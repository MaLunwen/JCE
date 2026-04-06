/*
 * jce_filesystem.c  Virtual file system implementation.
 *
 * Two backends:
 *   1. PhysFS: handles directories, zips, and other archives.
 *   2. PAK archive: wraps pak_find + pak_decompress.
 *
 * PhysFS mounts are checked first so developers can override
 * PAK-embedded assets without rebuilding.
 */

#include "jce_filesystem.h"
#include "jce_memory.h"
#include <jce/core/jce_log.h>

#include <jce/resource/pak_loader.h>
#include <physfs.h>

#include <string.h>

#define LOG_TAG "jce_fs"

/* ================================================================== */
/* Internal types                                                      */
/* ================================================================== */

struct JceFileSystem {
    PakArchive *pak;
    bool        physfs_owned;   /* true if we called PHYSFS_init */
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
    if (fs->physfs_owned && PHYSFS_isInit())
        PHYSFS_deinit();
    JCE_FREE(fs);
}

/* ================================================================== */
/* Mount points                                                        */
/* ================================================================== */

void jce_fs_mount_pak(JceFileSystem *fs, PakArchive *pak)
{
    if (fs) fs->pak = pak;
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

/* Try to open a virtual path from the PAK archive.
   Decompresses the entire asset into a memory buffer. */
static JceFile *try_open_pak(const JceFileSystem *fs, const char *vpath)
{
    if (!fs->pak) return NULL;

    const PakAsset *asset = pak_find(fs->pak, vpath);
    if (!asset) return NULL;

    /* Guard against uint64 → size_t truncation on 32-bit platforms. */
    if (asset->original_size > (uint64_t)SIZE_MAX) {
        LOG_ERROR(LOG_TAG, "asset too large for address space: %s", vpath);
        return NULL;
    }

    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) return NULL;

    size_t decompressed = pak_decompress(asset, buf, asset->original_size);
    if (decompressed == 0) {
        JCE_FREE(buf);
        return NULL;
    }

    JceFile *f = JCE_NEW(JceFile);
    if (!f) { JCE_FREE(buf); return NULL; }

    f->kind = JCE_FILE_PAK;
    f->u.pak.data   = buf;
    f->u.pak.size   = decompressed;
    f->u.pak.cursor = 0;
    return f;
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

size_t jce_fs_read(JceFile *file, void *buf, size_t buf_size)
{
    if (!file || !buf || buf_size == 0) return 0;

    if (file->kind == JCE_FILE_PHYSFS) {
        PHYSFS_sint64 n = PHYSFS_readBytes(file->u.physfs.handle,
                                           buf, (PHYSFS_uint64)buf_size);
        return (n > 0) ? (size_t)n : 0;
    }

    /* PAK: copy from the memory buffer. */
    size_t remaining = file->u.pak.size - file->u.pak.cursor;
    size_t to_read   = (buf_size < remaining) ? buf_size : remaining;
    memcpy(buf, (const char *)file->u.pak.data + file->u.pak.cursor, to_read);
    file->u.pak.cursor += to_read;
    return to_read;
}

size_t jce_fs_size(const JceFile *file)
{
    if (!file) return 0;
    if (file->kind == JCE_FILE_PHYSFS)
        return (file->u.physfs.total_size >= 0)
             ? (size_t)file->u.physfs.total_size : 0;
    return file->u.pak.size;
}

void *jce_fs_read_all(const JceFileSystem *fs, const char *virtual_path,
                      size_t *out_size)
{
    if (out_size) *out_size = 0;
    JceFile *f = jce_fs_open(fs, virtual_path);
    if (!f) return NULL;

    size_t sz = jce_fs_size(f);
    if (sz == 0) {
        jce_fs_close(f);
        return NULL;
    }

    void *buf = JCE_MALLOC(sz);
    if (!buf) { jce_fs_close(f); return NULL; }

    size_t nread = jce_fs_read(f, buf, sz);
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

    /* Check PAK. */
    if (fs->pak)
        return pak_find(fs->pak, virtual_path) != NULL;

    return false;
}
