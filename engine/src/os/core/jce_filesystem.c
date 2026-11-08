/*
 * jce_filesystem.c  Virtual file system implementation.
 *
 * Two backends:
 *   1. PhysFS: handles directories, zips, and other archives.
 *   2. PAK archive: wraps jce_pak_find + jce_pak_decompress.
 *
 * PhysFS mounts are checked first so developers can override
 * PAK-embedded assets without rebuilding.
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>   /* JceMutex — PhysFS is process-global + NOT thread-safe */

#include "jce_memory.h"
#include "jce_trace_scope_internal.h"
#include "jce_filesystem_read_internal.h"
#include "jce_filesystem_pak_internal.h"

#include <physfs.h>
#include <string.h>

#define LOG_TAG "jce_fs"

/* One serial owner for the active PAK provider and its registered native
 * admission tuple. Dependency inversion keeps native Pak details out of core;
 * exact tuple restoration retains the resource-owned predicate without an
 * opaque archive registry or a change to the public three-callback layout. */
static struct {
    const JceFsPakProvider *current_provider;
    JceFsPakProvider registered_provider;
    JceFsPakMountAllowedInternal registered_mount_allowed;
    JceFsPakMountAllowedInternal current_mount_allowed;
} s_pak_service;

/* PhysFS has process-global state (mount search path, open-handle list, error
 * code) and is explicitly NOT thread-safe.  The async world streamer + asset
 * loaders call jce_fs_open/read/close from worker threads concurrently, which
 * races PhysFS internals and corrupts the heap — manifesting as a wild-pointer
 * ACCESS_VIOLATION at the next allocation (e.g. inside cJSON during a chunk
 * parse).  Serialize ALL VFS access through one global lock, created on the
 * first jce_fs_create (main thread, before any worker runs) and intentionally
 * never destroyed (avoids a teardown use-after-free; a single leaked mutex).
 * Each PhysFS call is fully atomic under the lock; distinct JceFile handles are
 * independent, so interleaving open/read/close across threads is safe. */
static JceMutex *s_fs_mutex = NULL;
static uint32_t  s_fs_users = 0;
static bool      s_physfs_owned = false;
static inline void fs_lock(void)   { if (s_fs_mutex) jce_mutex_lock(s_fs_mutex); }
static inline void fs_unlock(void) { if (s_fs_mutex) jce_mutex_unlock(s_fs_mutex); }

void JCE_CALL jce_fs_set_pak_provider(const JceFsPakProvider *provider)
{
    s_pak_service.current_provider = provider;
    s_pak_service.current_mount_allowed = provider && s_pak_service.registered_mount_allowed &&
        provider->find == s_pak_service.registered_provider.find &&
        provider->asset_size == s_pak_service.registered_provider.asset_size &&
        provider->decompress == s_pak_service.registered_provider.decompress
            ? s_pak_service.registered_mount_allowed : NULL;
}

void JCE_CALL jce_fs_register_pak_provider_internal(
    const JceFsPakProvider *provider, JceFsPakMountAllowedInternal mount_allowed)
{
    if (!provider || !mount_allowed) return;
    s_pak_service.registered_provider = *provider;
    s_pak_service.registered_mount_allowed = mount_allowed;
    jce_fs_set_pak_provider(provider);
}

const JceFsPakProvider *JCE_CALL jce_fs_get_pak_provider(void)
{
    return s_pak_service.current_provider;
}

/* ================================================================== */
/* Internal types                                                      */
/* ================================================================== */

/* Single mounted PAK entry within the priority list.  When `name` is
 * NULL the entry was added through the legacy jce_fs_mount_pak() API
 * and represents the default monolithic archive. */
typedef struct JceFsPakEntry {
    JcePakArchive *pak;
    char          *name;     /* NULL or heap-allocated id (e.g. bundle id) */
} JceFsPakEntry;

#define JCE_FS_MAX_PAKS 64

struct JceFileSystem {
    JceFsPakEntry  paks[JCE_FS_MAX_PAKS];
    uint32_t       pak_count;
    /* Convenience alias to the legacy unnamed slot (or first slot if none).
     * Kept so static helpers below stay readable; not authoritative. */
    JcePakArchive *pak;
};

/* File opened from PhysFS. */
typedef struct {
    PHYSFS_File *handle;
    PHYSFS_sint64 total_size;
} PhysFSFile;

/* File opened from PAK (fully decompressed into memory). */
typedef struct {
    void   *data;
    size_t  size;
    size_t  cursor;
} PakFile;

typedef enum {
    JCE_FILE_PHYSFS,
    JCE_FILE_PAK
} JceFileKind;

struct JceFile {
    JceFileKind kind;
    union {
        PhysFSFile physfs;
        PakFile    pak;
    } u;
};

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

static bool fs_physfs_init(void)
{
#if JCE_PLATFORM_WEB
    return PHYSFS_init(NULL) != 0;
#else
    char argv0[1024];
    if (jce_fs_host_get_base_path(argv0, sizeof argv0)) {
        size_t length = strlen(argv0);

        if (length > 0 && argv0[length - 1] != '/' &&
            argv0[length - 1] != '\\' && length + 1 < sizeof argv0)
            argv0[length++] = '/';

        if (length + sizeof "jce" <= sizeof argv0) {
            memcpy(argv0 + length, "jce", sizeof "jce");
            return PHYSFS_init(argv0) != 0;
        }
    }

    return PHYSFS_init(NULL) != 0;
#endif
}

static JceFileSystem *jce_fs_create_impl(void)
{
    JceFileSystem *fs = JCE_NEW(JceFileSystem);
    if (!fs) return NULL;

    /* Create the global VFS lock once, here on the main thread, before any
     * streamer/asset worker can call into PhysFS. */
    if (!s_fs_mutex) s_fs_mutex = jce_mutex_create();
    if (!s_fs_mutex) {
        JCE_FREE(fs);
        return NULL; /* Never run PhysFS or mutate users without its lock. */
    }

    fs_lock();
    bool init_failed = false;
    const char *init_error = NULL;
    if (!PHYSFS_isInit()) {
        if (fs_physfs_init()) {
            s_physfs_owned = true;
        } else {
            init_failed = true;
            init_error = PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode());
        }
    }
    ++s_fs_users;
    fs_unlock();

    if (init_failed) {
        /* Embedded PAK reads remain available without PhysFS.  Every loose-file
         * entry point explicitly checks PHYSFS_isInit() before calling it. */
#if JCE_PLATFORM_WEB
        LOG_INFO(LOG_TAG,
                 "PhysFS unavailable under Emscripten — PAK-only mode "
                 "(loose-dir overrides are a native dev convenience)");
#else
        LOG_WARN(LOG_TAG,
                 "PHYSFS_init failed (%s) — PAK-only mode "
                 "(loose-dir overrides disabled)",
                 init_error ? init_error : "unknown error");
#endif
    }

    return fs;
}

static void jce_fs_destroy_impl(JceFileSystem *fs)
{
    if (!fs) return;
    for (uint32_t i = 0; i < fs->pak_count; ++i) {
        if (fs->paks[i].name) JCE_FREE(fs->paks[i].name);
    }

    fs_lock();
    if (s_fs_users > 0) --s_fs_users;
    if (s_fs_users == 0 && s_physfs_owned && PHYSFS_isInit()) {
        if (PHYSFS_deinit()) {
            s_physfs_owned = false;
        } else {
            LOG_WARN(LOG_TAG, "PHYSFS_deinit failed: %s",
                     PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        }
    }
    fs_unlock();
    JCE_FREE(fs);
}

/* ================================================================== */
/* Mount points                                                        */
/* ================================================================== */

static bool jce_fs_mount_pak_impl(JceFileSystem *fs, JcePakArchive *pak)
{
    if (!fs || !pak) return false;
    if (!s_pak_service.current_provider) return false;
    if (s_pak_service.current_mount_allowed && !s_pak_service.current_mount_allowed(pak)) return false;

    /* Replace the legacy unnamed slot if present; otherwise append. */
    for (uint32_t i = 0; i < fs->pak_count; ++i) {
        if (fs->paks[i].name == NULL) {
            fs->paks[i].pak = pak;
            fs->pak = pak;
            return true;
        }
    }

    if (fs->pak_count >= JCE_FS_MAX_PAKS) {
        LOG_WARN(LOG_TAG, "mount table full (%u); refusing legacy pak mount",
                 (unsigned)JCE_FS_MAX_PAKS);
        return false;
    }

    fs->paks[fs->pak_count].pak  = pak;
    fs->paks[fs->pak_count].name = NULL;
    ++fs->pak_count;
    fs->pak = pak;
    return true;
}

static bool jce_fs_mount_pak_named_impl(JceFileSystem *fs, const char *name,
                            JcePakArchive *pak)
{
    if (!fs || !pak || !name) return false;
    if (!s_pak_service.current_provider) return false;
    if (s_pak_service.current_mount_allowed && !s_pak_service.current_mount_allowed(pak)) return false;

    /* Reject duplicate names so refcounting in higher layers stays sane. */
    for (uint32_t i = 0; i < fs->pak_count; ++i) {
        if (fs->paks[i].name && strcmp(fs->paks[i].name, name) == 0) {
            LOG_WARN(LOG_TAG, "pak '%s' already mounted", name);
            return false;
        }
    }

    if (fs->pak_count >= JCE_FS_MAX_PAKS) {
        LOG_ERROR(LOG_TAG, "mount table full (%u); cannot mount '%s'",
                  (unsigned)JCE_FS_MAX_PAKS, name);
        return false;
    }

    /* Heap-copy the name (callers free their own strings). */
    size_t n = strlen(name) + 1;
    char *copy = (char *)JCE_MALLOC(n);
    if (!copy) return false;
    memcpy(copy, name, n);

    /* Insert at the FRONT (highest priority) so newer mounts win.  This
     * matches Unity AssetBundle behaviour where a freshly-mounted bundle
     * can shadow assets from older bundles. */
    for (uint32_t i = fs->pak_count; i > 0; --i)
        fs->paks[i] = fs->paks[i - 1];

    fs->paks[0].pak  = pak;
    fs->paks[0].name = copy;
    ++fs->pak_count;
    return true;
}

static bool jce_fs_unmount_pak_named_impl(JceFileSystem *fs, const char *name)
{
    if (!fs || !name) return false;

    for (uint32_t i = 0; i < fs->pak_count; ++i) {
        if (fs->paks[i].name && strcmp(fs->paks[i].name, name) == 0) {
            JCE_FREE(fs->paks[i].name);
            for (uint32_t j = i; j + 1 < fs->pak_count; ++j)
                fs->paks[j] = fs->paks[j + 1];
            --fs->pak_count;
            fs->paks[fs->pak_count].pak  = NULL;
            fs->paks[fs->pak_count].name = NULL;

            /* Re-pick `pak` alias if the legacy slot still exists. */
            fs->pak = NULL;
            for (uint32_t k = 0; k < fs->pak_count; ++k)
                if (fs->paks[k].name == NULL) { fs->pak = fs->paks[k].pak; break; }
            return true;
        }
    }
    return false;
}

uint32_t jce_fs_mounted_pak_count(const JceFileSystem *fs)
{
    return fs ? fs->pak_count : 0;
}

static bool jce_fs_mount_dir_impl(JceFileSystem *fs, const char *prefix,
                      const char *directory)
{
    if (!fs || !directory) return false;

    /* PhysFS mount: mountPoint is the virtual prefix. */
    fs_lock();
    bool initialized = PHYSFS_isInit() != 0;
    bool mounted = initialized && PHYSFS_mount(directory, prefix, 1);
    const char *error = mounted || !initialized ? NULL :
        PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode());
    fs_unlock();
    if (!mounted) {
        LOG_WARN(LOG_TAG, "PHYSFS_mount('%s' -> '%s') failed: %s",
                 directory, prefix ? prefix : "/",
                 error ? error : "PhysFS is not initialized");
        return false;
    }

    LOG_DEBUG(LOG_TAG, "mounted dir '%s' -> '%s'",
              prefix ? prefix : "/", directory);
    return true;
}

/* ================================================================== */
/* PhysFS file helpers                                                 */
/* ================================================================== */

static JceFile *try_open_physfs(const char *vpath)
{
    if (!PHYSFS_isInit()) return NULL;

    PHYSFS_File *h = PHYSFS_openRead(vpath);
    if (!h) return NULL;

    JceFile *f = JCE_NEW(JceFile);
    if (!f) { PHYSFS_close(h); return NULL; }

    f->kind = JCE_FILE_PHYSFS;
    f->u.physfs.handle     = h;
    f->u.physfs.total_size = PHYSFS_fileLength(h);
    return f;
}

/* Try to open a virtual path from any mounted PAK archive.
   Walks the priority list (front → back) and returns the first hit. */
static JceFile *try_open_pak(const JceFileSystem *fs, const char *vpath,
                             uint64_t max_bytes, uint64_t *out_size)
{
    if (!fs || fs->pak_count == 0) return NULL;
    const JceFsPakProvider *prov = s_pak_service.current_provider;
    if (!prov || !prov->find) return NULL;   /* no resource provider installed */

    for (uint32_t i = 0; i < fs->pak_count; ++i) {
        JcePakArchive *p = fs->paks[i].pak;
        if (!p) continue;

        const void *asset = prov->find(p, vpath);
        if (!asset) continue;

        uint64_t orig = prov->asset_size(asset);
        if (out_size) *out_size = orig;
        /* PAK open eagerly decompresses; enforce a bound before allocation. */
        if (max_bytes && (orig > max_bytes || orig >= (uint64_t)SIZE_MAX))
            return NULL;
        if (orig > (uint64_t)SIZE_MAX) {
            LOG_ERROR(LOG_TAG, "asset too large for address space: %s", vpath);
            return NULL;
        }

        void *buf = JCE_MALLOC((size_t)orig);
        if (!buf) return NULL;

        size_t decompressed = prov->decompress(asset, buf, (size_t)orig);
        if (max_bytes && decompressed != (size_t)orig) {
            JCE_FREE(buf);
            return NULL;
        }
        if (decompressed == 0) {
            JCE_FREE(buf);
            continue; /* try next pak — corrupt entry shouldn't kill lookup */
        }

        JceFile *f = JCE_NEW(JceFile);
        if (!f) { JCE_FREE(buf); return NULL; }

        f->kind = JCE_FILE_PAK;
        f->u.pak.data   = buf;
        f->u.pak.size   = decompressed;
        f->u.pak.cursor = 0;
        return f;
    }
    return NULL;
}

/* ================================================================== */
/* File operations                                                     */
/* ================================================================== */

static JceFile *jce_fs_open_impl(const JceFileSystem *fs, const char *virtual_path)
{
    if (!fs || !virtual_path) return NULL;

    /* PhysFS first (developer override via mounted dirs/archives), then PAKs.
     * The whole lookup runs under the VFS lock: PHYSFS_openRead and the PAK
     * decompress path are both non-thread-safe and are called concurrently from
     * streamer/asset worker threads. */
    fs_lock();
    JceFile *f = try_open_physfs(virtual_path);
    if (!f) f = try_open_pak(fs, virtual_path, 0, NULL);
    fs_unlock();
    return f;
}

static void jce_fs_close_impl(JceFile *file)
{
    if (!file) return;
    if (file->kind == JCE_FILE_PHYSFS) {
        fs_lock();
        PHYSFS_close(file->u.physfs.handle);
        fs_unlock();
    } else {
        JCE_FREE(file->u.pak.data);   /* PAK file owns a private buffer — no lock */
    }
    JCE_FREE(file);
}

uint64_t jce_fs_read(JceFile *file, void *buf, uint64_t buf_size)
{
    if (!file || !buf || buf_size == 0) return 0;

    if (file->kind == JCE_FILE_PHYSFS) {
        fs_lock();
        PHYSFS_sint64 n = PHYSFS_readBytes(file->u.physfs.handle,
                                           buf, (PHYSFS_uint64)buf_size);
        fs_unlock();
        return (n > 0) ? (uint64_t)n : 0;
    }

    /* PAK: copy from the memory buffer. */
    uint64_t remaining = file->u.pak.size - file->u.pak.cursor;
    uint64_t to_read   = (buf_size < remaining) ? buf_size : remaining;
    memcpy(buf, (const char *)file->u.pak.data + file->u.pak.cursor, (size_t)to_read);
    file->u.pak.cursor += to_read;
    return to_read;
}

uint64_t jce_fs_size(const JceFile *file)
{
    if (!file) return 0;
    if (file->kind == JCE_FILE_PHYSFS)
        return (file->u.physfs.total_size >= 0)
             ? (uint64_t)file->u.physfs.total_size : 0;
    return file->u.pak.size;
}

static void *jce_fs_read_all_impl(const JceFileSystem *fs, const char *virtual_path,
                      uint64_t *out_size)
{
    if (out_size) *out_size = 0;
    uint64_t observed = 0;
    /* One bounded whole-asset owner: reject lengths without representable
     * sentinel headroom before PAK eager allocation or PhysFS allocation.
     * Preserve public failure out_size=0; the internal reader independently
     * retains an observed total on rejection. Returned PAK data is binary,
     * without a general NUL-termination promise. */
    void *result = jce_fs_read_all_bounded_internal(fs, virtual_path,
                                                   (uint64_t)SIZE_MAX - 1u, &observed);
    if (result && out_size) *out_size = observed;
    return result;
}

JceFsPakReadResult JCE_CALL jce_fs_read_pak_bounded(
    const JceFileSystem *fs, const char *key, uint64_t max_bytes,
    void **out_data, uint64_t *out_size)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.read_pak_bounded");
    JceFsPakReadResult result = JCE_FS_PAK_READ_FAILED;
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    if (!fs || !key || !key[0] || !max_bytes) goto done;
    fs_lock();
    const JceFsPakProvider *provider = s_pak_service.current_provider;
    if (fs->pak_count == 0) { result = JCE_FS_PAK_READ_ABSENT; goto unlock; }
    if (!provider || !provider->find || !provider->asset_size ||
        !provider->decompress) goto unlock;
    result = JCE_FS_PAK_READ_ABSENT;
    for (uint32_t i = 0; i < fs->pak_count; ++i) {
        if (!fs->paks[i].pak) continue;
        const void *asset = provider->find(fs->paks[i].pak, key);
        if (!asset) continue;
        const uint64_t size = provider->asset_size(asset);
        result = JCE_FS_PAK_READ_OVER_LIMIT;
        if (size > max_bytes || size >= (uint64_t)SIZE_MAX) break;
        result = JCE_FS_PAK_READ_FAILED;
        if (out_data) {
            void *data = JCE_MALLOC((size_t)size + 1u);
            if (!data) break;
            if (provider->decompress(asset, data, (size_t)size) != (size_t)size) {
                JCE_FREE(data);
                break;
            }
            ((char *)data)[size] = '\0';
            *out_data = data;
        }
        if (out_size) *out_size = size;
        result = JCE_FS_PAK_READ_OK;
        break;
    }
unlock:
    fs_unlock();
done:
    jce_trace_scope_end(scope, result == JCE_FS_PAK_READ_OK ? JCE_TRACE_TASK_SUCCEEDED :
        result == JCE_FS_PAK_READ_ABSENT ? JCE_TRACE_TASK_CANCELLED : JCE_TRACE_TASK_FAILED);
    return result;
}

void *jce_fs_read_all_bounded_internal(const JceFileSystem *fs,
    const char *path, uint64_t max_bytes, uint64_t *out_size)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.read_all_bounded");
    void *buffer = NULL;
    JceFile *file = NULL;
    uint64_t total = 0;
    if (out_size) *out_size = 0;
    if (!fs || !path || !path[0] || !max_bytes) goto done;
    fs_lock();
    file = try_open_physfs(path);
    if (!file) file = try_open_pak(fs, path, max_bytes, &total);
    fs_unlock();
    if (!file) goto done;
    total = jce_fs_size(file);
    if (!total || total > max_bytes || total >= (uint64_t)SIZE_MAX) goto done;
    if (file->kind == JCE_FILE_PAK) {
        /* Transfer the eagerly decoded file buffer, avoiding a second copy. */
        buffer = file->u.pak.data;
        file->u.pak.data = NULL;
    } else {
        buffer = JCE_MALLOC((size_t)total + 1u);
        if (buffer && jce_fs_read(file, buffer, total) != total) {
            JCE_FREE(buffer);
            buffer = NULL;
        }
        if (buffer) ((char *)buffer)[total] = '\0';
    }
done:
    if (file) jce_fs_close(file);
    if (out_size) *out_size = total;
    jce_trace_scope_end(scope, buffer ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return buffer;
}

bool jce_fs_exists(const JceFileSystem *fs, const char *virtual_path)
{
    if (!fs || !virtual_path) return false;

    fs_lock();
    /* Check PhysFS search path, then every mounted PAK (via the provider). */
    bool found = PHYSFS_isInit() && PHYSFS_exists(virtual_path);
    if (!found) {
        const JceFsPakProvider *prov = s_pak_service.current_provider;
        if (prov && prov->find) {
            for (uint32_t i = 0; i < fs->pak_count; ++i) {
                if (fs->paks[i].pak &&
                    prov->find(fs->paks[i].pak, virtual_path) != NULL) {
                    found = true;
                    break;
                }
            }
        }
    }
    fs_unlock();
    return found;
}

static void fs_probe_search_path_internal(void *data, const char *path)
{
    (void)path;
    *(bool *)data = true;
}

/* Metadata only, preserving the same PhysFS-before-PAK priority as reads.
 * An uncertain higher-priority source never becomes a fallback absence. */
static JceFsPresence fs_probe_vfs_internal(const JceFileSystem *fs, const char *path)
{
    if (!fs || !path || !path[0] || !s_fs_mutex) return JCE_FS_PRESENCE_UNKNOWN;
    JceFsPresence presence = JCE_FS_PRESENCE_ABSENT;
    fs_lock();
    if (PHYSFS_isInit()) {
        bool has_search_path = false;
        PHYSFS_getSearchPathCallback(fs_probe_search_path_internal, &has_search_path);
        /* Stat on an empty search path returns zero without setting an error.
         * Classify only this call, never an unrelated prior TLS error. */
        (void)PHYSFS_getLastErrorCode();
        PHYSFS_Stat info;
        if (PHYSFS_stat(path, &info)) {
            presence = info.filetype == PHYSFS_FILETYPE_REGULAR
                ? JCE_FS_PRESENCE_PRESENT : JCE_FS_PRESENCE_UNKNOWN;
        } else {
            const PHYSFS_ErrorCode error = PHYSFS_getLastErrorCode();
            if (error != PHYSFS_ERR_NOT_FOUND &&
                !(error == PHYSFS_ERR_OK && !has_search_path))
                presence = JCE_FS_PRESENCE_UNKNOWN;
        }
    }
    if (presence == JCE_FS_PRESENCE_ABSENT && fs->pak_count) {
        const JceFsPakProvider *provider = s_pak_service.current_provider;
        if (!provider || !provider->find || !provider->asset_size || !provider->decompress)
            presence = JCE_FS_PRESENCE_UNKNOWN;
        else for (uint32_t n = 0; n < fs->pak_count; ++n) {
            if (!fs->paks[n].pak) { presence = JCE_FS_PRESENCE_UNKNOWN; break; }
            if (provider->find(fs->paks[n].pak, path)) { presence = JCE_FS_PRESENCE_PRESENT; break; }
        }
    }
    fs_unlock();
    return presence;
}

/* ================================================================== */
/* Write support                                                       */
/* ================================================================== */

static bool jce_fs_set_write_dir_impl(JceFileSystem *fs, const char *directory)
{
    if (!fs) return false;

    fs_lock();
    if (!PHYSFS_isInit()) {
        fs_unlock();
        return !directory || directory[0] == '\0';
    }

    if (!directory || directory[0] == '\0') {
        /* Clear the write directory. */
        PHYSFS_setWriteDir(NULL);
        fs_unlock();
        return true;
    }

    if (!PHYSFS_setWriteDir(directory)) {
        const char *error = PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode());
        fs_unlock();
        LOG_ERROR(LOG_TAG, "PHYSFS_setWriteDir('%s') failed: %s",
                  directory, error ? error : "unknown error");
        return false;
    }
    fs_unlock();

    LOG_DEBUG(LOG_TAG, "write dir set to '%s'", directory);
    return true;
}

static JceFile *jce_fs_open_write_impl(JceFileSystem *fs, const char *virtual_path)
{
    if (!fs || !virtual_path) return NULL;

    fs_lock();
    if (!PHYSFS_isInit()) {
        fs_unlock();
        return NULL;
    }

    PHYSFS_File *h = PHYSFS_openWrite(virtual_path);
    if (!h) {
        const char *error = PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode());
        fs_unlock();
        LOG_ERROR(LOG_TAG, "PHYSFS_openWrite('%s') failed: %s",
                  virtual_path, error ? error : "unknown error");
        return NULL;
    }

    JceFile *f = JCE_NEW(JceFile);
    if (!f) {
        PHYSFS_close(h);
        fs_unlock();
        return NULL;
    }

    f->kind = JCE_FILE_PHYSFS;
    f->u.physfs.handle     = h;
    f->u.physfs.total_size = 0;
    fs_unlock();
    return f;
}

uint64_t jce_fs_write(JceFile *file, const void *buf, uint64_t size)
{
    if (!file || !buf || size == 0) return 0;

    if (file->kind != JCE_FILE_PHYSFS) {
        LOG_ERROR(LOG_TAG, "cannot write to a PAK-backed file");
        return 0;
    }

    fs_lock();
    PHYSFS_sint64 n = PHYSFS_writeBytes(file->u.physfs.handle,
                                        buf, (PHYSFS_uint64)size);
    fs_unlock();
    return (n > 0) ? (uint64_t)n : 0;
}

static bool jce_fs_write_all_impl(JceFileSystem *fs, const char *virtual_path,
                      const void *data, uint64_t size)
{
    if (!fs || !virtual_path || !data) return false;

    JceFile *f = jce_fs_open_write(fs, virtual_path);
    if (!f) return false;

    uint64_t written = jce_fs_write(f, data, size);
    jce_fs_close(f);

    if (written != size) {
        LOG_ERROR(LOG_TAG, "write error: wrote %llu / %llu bytes to '%s'",
                  (unsigned long long)written, (unsigned long long)size, virtual_path);
        return false;
    }

    return true;
}

/* ================================================================== */
/* Active VFS override                                                  */
/*                                                                      */
/* Single-process editor preview hook.  A scene loaded from a mounted   */
/* bundle parks its JceFileSystem* here so that asset cache loaders     */
/* (which call jce_fs_host_read_all with project-relative paths)        */
/* transparently resolve through the bundle VFS without changing every  */
/* loader.  See jce_fs_host_read_all in jce_filesystem_host.c.          */
/* ================================================================== */

/* ================================================================== */
/* Active VFS override                                                  */
/*                                                                      */
/* Convenience: install jce_fs_read_all as the active reader.  Storage  */
/* of the active fs pointer and reader fn lives in jce_filesystem_host.c*/
/* (so consumers that link only that TU still get a working stub).      */
/* ================================================================== */

void jce__fs_store_active_policy(JceFsActivePolicy policy);

void jce_fs_set_active(JceFileSystem *fs)
{
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_OVERLAY);
}

void jce_fs_set_active_policy(JceFileSystem *fs, JceFsActivePolicy policy)
{
    if (policy != JCE_FS_ACTIVE_ISOLATED)
        policy = JCE_FS_ACTIVE_OVERLAY;
    jce_fs_set_active_reader(fs ? jce_fs_read_all : NULL);
    /* Store the handle and fallback policy through host-side helpers. */
    jce__fs_store_active(fs, fs ? fs_probe_vfs_internal : NULL);
    jce__fs_store_active_policy(fs ? policy : JCE_FS_ACTIVE_OVERLAY);
}

/* Trace adapters keep IO behavior in the existing implementation owner.
 * Read/query APIs with ambiguous zero/EOF results are not labelled successful
 * here. Capture-owned host IO is suppressed only on its consumer thread. */
JceFileSystem *jce_fs_create(void)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.create");
    JceFileSystem *result = jce_fs_create_impl();
    jce_trace_scope_end(scope, (result != NULL) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

void jce_fs_destroy(JceFileSystem *fs)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.destroy");
    jce_fs_destroy_impl(fs);
    jce_trace_scope_end(scope, fs ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_CANCELLED);
}

bool jce_fs_mount_pak(JceFileSystem *fs, JcePakArchive *pak)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.mount_pak");
    bool result = jce_fs_mount_pak_impl(fs, pak);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_mount_pak_named(JceFileSystem *fs, const char *name,
                            JcePakArchive *pak)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.mount_pak_named");
    bool result = jce_fs_mount_pak_named_impl(fs, name, pak);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_unmount_pak_named(JceFileSystem *fs, const char *name)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.unmount_pak_named");
    bool result = jce_fs_unmount_pak_named_impl(fs, name);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_mount_dir(JceFileSystem *fs, const char *prefix,
                      const char *directory)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.mount_dir");
    bool result = jce_fs_mount_dir_impl(fs, prefix, directory);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

JceFile *jce_fs_open(const JceFileSystem *fs, const char *virtual_path)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.open");
    JceFile *result = jce_fs_open_impl(fs, virtual_path);
    jce_trace_scope_end(scope, (result != NULL) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

void jce_fs_close(JceFile *file)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.close");
    jce_fs_close_impl(file);
    jce_trace_scope_end(scope, file ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_CANCELLED);
}

void *jce_fs_read_all(const JceFileSystem *fs, const char *virtual_path,
                      uint64_t *out_size)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.read_all");
    void *result = jce_fs_read_all_impl(fs, virtual_path, out_size);
    jce_trace_scope_end(scope, (result != NULL) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_set_write_dir(JceFileSystem *fs, const char *directory)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.set_write_dir");
    bool result = jce_fs_set_write_dir_impl(fs, directory);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

JceFile *jce_fs_open_write(JceFileSystem *fs, const char *virtual_path)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.open_write");
    JceFile *result = jce_fs_open_write_impl(fs, virtual_path);
    jce_trace_scope_end(scope, (result != NULL) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}

bool jce_fs_write_all(JceFileSystem *fs, const char *virtual_path,
                      const void *data, uint64_t size)
{
    JceTraceScope scope = jce_trace_scope_begin("fs.write_all");
    bool result = jce_fs_write_all_impl(fs, virtual_path, data, size);
    jce_trace_scope_end(scope, (result) ? JCE_TRACE_TASK_SUCCEEDED
                         : JCE_TRACE_TASK_FAILED);
    return result;
}
