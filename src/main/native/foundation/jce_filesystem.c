/*
 * jce_filesystem.c  Virtual file system implementation.
 *
 * Two backends:
 *   1. Loose-file (directory mount): wraps fopen/fread, for dev overlay.
 *   2. PAK archive: wraps pak_find + pak_decompress.
 *
 * Loose-file mounts are checked first so developers can override
 * PAK-embedded assets without rebuilding.
 */

#include "jce_filesystem.h"
#include "jce_memory.h"
#include "jce_log.h"

#include "resource/pak_loader.h"

#include <stdio.h>
#include <string.h>

#define LOG_TAG "jce_fs"

/* ================================================================== */
/* Internal types                                                      */
/* ================================================================== */

#define JCE_FS_MAX_DIR_MOUNTS 8

typedef struct {
    char prefix[128];
    char directory[512];
} DirMount;

struct JceFileSystem {
    PakArchive *pak;
    DirMount    dirs[JCE_FS_MAX_DIR_MOUNTS];
    int         dir_count;
};

/* File opened from loose filesystem. */
typedef struct {
    FILE  *fp;
    size_t total_size;
} LooseFile;

/* File opened from PAK (fully decompressed into memory). */
typedef struct {
    void   *data;
    size_t  size;
    size_t  cursor;
} PakFile;

typedef enum {
    JCE_FILE_LOOSE,
    JCE_FILE_PAK
} JceFileKind;

struct JceFile {
    JceFileKind kind;
    union {
        LooseFile loose;
        PakFile   pak;
    } u;
};

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JceFileSystem *jce_fs_create(void)
{
    return JCE_NEW(JceFileSystem);
}

void jce_fs_destroy(JceFileSystem *fs)
{
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
    if (!fs || !prefix || !directory) return;
    if (fs->dir_count >= JCE_FS_MAX_DIR_MOUNTS) {
        LOG_WARN(LOG_TAG, "max directory mounts reached (%d)",
                 JCE_FS_MAX_DIR_MOUNTS);
        return;
    }

    DirMount *dm = &fs->dirs[fs->dir_count++];
    snprintf(dm->prefix, sizeof(dm->prefix), "%s", prefix);
    snprintf(dm->directory, sizeof(dm->directory), "%s", directory);

    LOG_DEBUG(LOG_TAG, "mounted dir '%s' -> '%s'", prefix, directory);
}

/* ================================================================== */
/* Loose-file helpers                                                  */
/* ================================================================== */

/* Try to open a virtual path via loose-file mounts.
   Returns a JceFile on success, or NULL. */
static JceFile *try_open_loose(const JceFileSystem *fs, const char *vpath)
{
    for (int i = 0; i < fs->dir_count; i++) {
        const DirMount *dm = &fs->dirs[i];
        size_t plen = strlen(dm->prefix);

        if (strncmp(vpath, dm->prefix, plen) != 0)
            continue;

        /* Build real path: directory + remainder after prefix. */
        char real_path[1024];
        snprintf(real_path, sizeof(real_path), "%s%s",
                 dm->directory, vpath + plen);

        FILE *fp = fopen(real_path, "rb");
        if (!fp) continue;

        /* Determine size. */
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        JceFile *f = JCE_NEW(JceFile);
        if (!f) { fclose(fp); return NULL; }

        f->kind = JCE_FILE_LOOSE;
        f->u.loose.fp = fp;
        f->u.loose.total_size = (sz > 0) ? (size_t)sz : 0;
        return f;
    }
    return NULL;
}

/* Try to open a virtual path from the PAK archive.
   Decompresses the entire asset into a memory buffer. */
static JceFile *try_open_pak(const JceFileSystem *fs, const char *vpath)
{
    if (!fs->pak) return NULL;

    const PakAsset *asset = pak_find(fs->pak, vpath);
    if (!asset) return NULL;

    void *buf = JCE_MALLOC(asset->original_size);
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

    /* Loose files first (developer override). */
    JceFile *f = try_open_loose(fs, virtual_path);
    if (f) return f;

    return try_open_pak(fs, virtual_path);
}

void jce_fs_close(JceFile *file)
{
    if (!file) return;
    if (file->kind == JCE_FILE_LOOSE) {
        fclose(file->u.loose.fp);
    } else {
        JCE_FREE(file->u.pak.data);
    }
    JCE_FREE(file);
}

size_t jce_fs_read(JceFile *file, void *buf, size_t buf_size)
{
    if (!file || !buf || buf_size == 0) return 0;

    if (file->kind == JCE_FILE_LOOSE) {
        return fread(buf, 1, buf_size, file->u.loose.fp);
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
    if (file->kind == JCE_FILE_LOOSE)
        return file->u.loose.total_size;
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

    /* Check loose mounts. */
    for (int i = 0; i < fs->dir_count; i++) {
        const DirMount *dm = &fs->dirs[i];
        size_t plen = strlen(dm->prefix);
        if (strncmp(virtual_path, dm->prefix, plen) != 0)
            continue;

        char real_path[1024];
        snprintf(real_path, sizeof(real_path), "%s%s",
                 dm->directory, virtual_path + plen);

        FILE *fp = fopen(real_path, "rb");
        if (fp) { fclose(fp); return true; }
    }

    /* Check PAK. */
    if (fs->pak)
        return pak_find(fs->pak, virtual_path) != NULL;

    return false;
}
