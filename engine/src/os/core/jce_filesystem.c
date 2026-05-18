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
#include <jce/resource/jce_pak_loader.h>

#include "jce_memory.h"

#include <physfs.h>
#include <string.h>

#define LOG_TAG "jce_fs"

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
    bool           physfs_owned;   /* true if we called PHYSFS_init */
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

JceFileSystem *jce_fs_create(void)
{
    JceFileSystem *fs = JCE_NEW(JceFileSystem);
    if (!fs) return NULL;

    if (!PHYSFS_isInit()) {
        if (!PHYSFS_init(NULL)) {
            LOG_ERROR(LOG_TAG, "PHYSFS_init failed: %s",
                      PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
            JCE_FREE(fs);
            return NULL;
        }
        fs->physfs_owned = true;
    }

    return fs;
}

void jce_fs_destroy(JceFileSystem *fs)
{
    if (!fs) return;
    for (uint32_t i = 0; i < fs->pak_count; ++i) {
        if (fs->paks[i].name) JCE_FREE(fs->paks[i].name);
    }
    if (fs->physfs_owned && PHYSFS_isInit())
        PHYSFS_deinit();
    JCE_FREE(fs);
}

/* ================================================================== */
/* Mount points                                                        */
/* ================================================================== */

void jce_fs_mount_pak(JceFileSystem *fs, JcePakArchive *pak)
{
    if (!fs) return;

    /* Replace the legacy unnamed slot if present; otherwise append. */
    for (uint32_t i = 0; i < fs->pak_count; ++i) {
        if (fs->paks[i].name == NULL) {
            fs->paks[i].pak = pak;
            fs->pak = pak;
            return;
        }
    }

    if (fs->pak_count >= JCE_FS_MAX_PAKS) {
        LOG_WARN(LOG_TAG, "mount table full (%u); refusing legacy pak mount",
                 (unsigned)JCE_FS_MAX_PAKS);
        return;
    }

    fs->paks[fs->pak_count].pak  = pak;
    fs->paks[fs->pak_count].name = NULL;
    ++fs->pak_count;
    fs->pak = pak;
}

bool jce_fs_mount_pak_named(JceFileSystem *fs, const char *name,
                            JcePakArchive *pak)
{
    if (!fs || !pak || !name) return false;

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

bool jce_fs_unmount_pak_named(JceFileSystem *fs, const char *name)
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

void jce_fs_mount_dir(JceFileSystem *fs, const char *prefix,
                      const char *directory)
{
    if (!fs || !directory) return;

    /* PhysFS mount: mountPoint is the virtual prefix. */
    if (!PHYSFS_mount(directory, prefix, 1)) {
        LOG_WARN(LOG_TAG, "PHYSFS_mount('%s' -> '%s') failed: %s",
                 directory, prefix ? prefix : "/",
                 PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return;
    }

    LOG_DEBUG(LOG_TAG, "mounted dir '%s' -> '%s'",
              prefix ? prefix : "/", directory);
}

/* ================================================================== */
/* PhysFS file helpers                                                 */
/* ================================================================== */

static JceFile *try_open_physfs(const char *vpath)
{
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
static JceFile *try_open_pak(const JceFileSystem *fs, const char *vpath)
{
    if (!fs || fs->pak_count == 0) return NULL;

    for (uint32_t i = 0; i < fs->pak_count; ++i) {
        JcePakArchive *p = fs->paks[i].pak;
        if (!p) continue;

        const JcePakAsset *asset = jce_pak_find(p, vpath);
        if (!asset) continue;

        if (asset->original_size > (uint64_t)SIZE_MAX) {
            LOG_ERROR(LOG_TAG, "asset too large for address space: %s", vpath);
            return NULL;
        }

        void *buf = JCE_MALLOC((size_t)asset->original_size);
        if (!buf) return NULL;

        size_t decompressed = jce_pak_decompress(asset, buf,
                                                 asset->original_size);
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

JceFile *jce_fs_open(const JceFileSystem *fs, const char *virtual_path)
{
    if (!fs || !virtual_path) return NULL;

    /* PhysFS first (developer override via mounted dirs/archives). */
    JceFile *f = try_open_physfs(virtual_path);
    if (f) return f;

    return try_open_pak(fs, virtual_path);
}

void jce_fs_close(JceFile *file)
{
    if (!file) return;
    if (file->kind == JCE_FILE_PHYSFS) {
        PHYSFS_close(file->u.physfs.handle);
    } else {
        JCE_FREE(file->u.pak.data);
    }
    JCE_FREE(file);
}

uint64_t jce_fs_read(JceFile *file, void *buf, uint64_t buf_size)
{
    if (!file || !buf || buf_size == 0) return 0;

    if (file->kind == JCE_FILE_PHYSFS) {
        PHYSFS_sint64 n = PHYSFS_readBytes(file->u.physfs.handle,
                                           buf, (PHYSFS_uint64)buf_size);
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

void *jce_fs_read_all(const JceFileSystem *fs, const char *virtual_path,
                      uint64_t *out_size)
{
    if (out_size) *out_size = 0;
    JceFile *f = jce_fs_open(fs, virtual_path);
    if (!f) return NULL;

    uint64_t sz = jce_fs_size(f);
    if (sz == 0) {
        jce_fs_close(f);
        return NULL;
    }

    void *buf = JCE_MALLOC(sz);
    if (!buf) { jce_fs_close(f); return NULL; }

    uint64_t nread = jce_fs_read(f, buf, sz);
    jce_fs_close(f);

    if (nread != sz) {
        JCE_FREE(buf);
        return NULL;
    }

    if (out_size) *out_size = nread;
    return buf;
}

bool jce_fs_exists(const JceFileSystem *fs, const char *virtual_path)
{
    if (!fs || !virtual_path) return false;

    /* Check PhysFS search path. */
    if (PHYSFS_exists(virtual_path))
        return true;

    /* Check every mounted PAK. */
    for (uint32_t i = 0; i < fs->pak_count; ++i) {
        if (fs->paks[i].pak &&
            jce_pak_find(fs->paks[i].pak, virtual_path) != NULL)
            return true;
    }
    return false;
}

/* ================================================================== */
/* Write support                                                       */
/* ================================================================== */

bool jce_fs_set_write_dir(JceFileSystem *fs, const char *directory)
{
    if (!fs) return false;

    if (!directory || directory[0] == '\0') {
        /* Clear the write directory. */
        PHYSFS_setWriteDir(NULL);
        return true;
    }

    if (!PHYSFS_setWriteDir(directory)) {
        LOG_ERROR(LOG_TAG, "PHYSFS_setWriteDir('%s') failed: %s",
                  directory,
                  PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return false;
    }

    LOG_DEBUG(LOG_TAG, "write dir set to '%s'", directory);
    return true;
}

JceFile *jce_fs_open_write(JceFileSystem *fs, const char *virtual_path)
{
    if (!fs || !virtual_path) return NULL;

    PHYSFS_File *h = PHYSFS_openWrite(virtual_path);
    if (!h) {
        LOG_ERROR(LOG_TAG, "PHYSFS_openWrite('%s') failed: %s",
                  virtual_path,
                  PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        return NULL;
    }

    JceFile *f = JCE_NEW(JceFile);
    if (!f) { PHYSFS_close(h); return NULL; }

    f->kind = JCE_FILE_PHYSFS;
    f->u.physfs.handle     = h;
    f->u.physfs.total_size = 0;
    return f;
}

uint64_t jce_fs_write(JceFile *file, const void *buf, uint64_t size)
{
    if (!file || !buf || size == 0) return 0;

    if (file->kind != JCE_FILE_PHYSFS) {
        LOG_ERROR(LOG_TAG, "cannot write to a PAK-backed file");
        return 0;
    }

    PHYSFS_sint64 n = PHYSFS_writeBytes(file->u.physfs.handle,
                                         buf, (PHYSFS_uint64)size);
    return (n > 0) ? (uint64_t)n : 0;
}

bool jce_fs_write_all(JceFileSystem *fs, const char *virtual_path,
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

void jce_fs_set_active(JceFileSystem *fs)
{
    jce_fs_set_active_reader(fs ? jce_fs_read_all : NULL);
    /* Store the handle through the host-side setter via a small helper. */
    extern void jce__fs_store_active(JceFileSystem *fs);
    jce__fs_store_active(fs);
}
