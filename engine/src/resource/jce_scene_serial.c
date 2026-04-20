/*
 * jce_scene_serial.c  Resource-layer scene I/O (thin wrapper).
 *
 * Single source of truth for component (de)serialization lives in the
 * scene layer (engine/src/scene/jce_scene_serial.c). This module only
 * adds:
 *   1. Cross-platform file I/O (PhysFS + SDL fallback).
 *   2. VFS-aware load via JceFileSystem.
 *   3. The legacy `jce_scene_serial_*` C API still consumed by the
 *      editor's history/snapshot system and by ck.
 *
 * Dogfooding: editor saves and ck loads through the same JSON parser,
 * so the on-disk format can never diverge between tools.
 */

#include <jce/resource/jce_scene_serial.h>
#include <jce/resource/jce_scene_contract.h>
#include <jce/scene/jce_scene.h>
#include <jce/scene/jce_scene_serial.h>      /* canonical (de)serializer */
#include <jce/core/jce_filesystem.h>
#include <jce/core/jce_log.h>

#include <physfs.h>
#include <cjson/cJSON.h>
#include <SDL3/SDL.h>
#include <string.h>
#include "core/jce_memory.h"

#define LOG_TAG "scene_serial"

/* ── Save ─────────────────────────────────────────────────────────── */

char *jce_scene_serial_save(const JceScene *scene, size_t *out_len)
{
    if (!scene) return NULL;

    cJSON *root = jce_scene_save_json(scene);
    if (!root) return NULL;

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (out_len && json)
        *out_len = strlen(json);
    return json;
}

bool jce_scene_serial_save_file(const JceScene *scene, const char *path)
{
    if (!scene || !path) return false;

    size_t len = 0;
    char *json = jce_scene_serial_save(scene, &len);
    if (!json) return false;

    /* Use PhysFS for cross-platform I/O. PhysFS write requires a write
     * directory; if none has been configured we extract the directory
     * part of `path` and set it temporarily. The filename (leaf) is
     * used as the PhysFS virtual path. */
    const char *sep = strrchr(path, '/');
    if (!sep) sep = strrchr(path, '\\');

    char dir_buf[1024];
    const char *filename = path;
    if (sep) {
        size_t dir_len = (size_t)(sep - path);
        if (dir_len >= sizeof(dir_buf)) dir_len = sizeof(dir_buf) - 1;
        memcpy(dir_buf, path, dir_len);
        dir_buf[dir_len] = '\0';
        filename = sep + 1;
    } else {
        dir_buf[0] = '.';
        dir_buf[1] = '\0';
    }

    /* Save and restore the previous write dir (may be unset). */
    const char *prev_write_dir = NULL;
    char prev_buf[1024] = {0};
    if (PHYSFS_isInit()) {
        prev_write_dir = PHYSFS_getWriteDir();
        if (prev_write_dir) {
            size_t plen = strlen(prev_write_dir);
            if (plen < sizeof(prev_buf))
                memcpy(prev_buf, prev_write_dir, plen + 1);
        }
    }

    if (PHYSFS_isInit() && PHYSFS_setWriteDir(dir_buf)) {
        PHYSFS_File *fh = PHYSFS_openWrite(filename);
        if (fh) {
            PHYSFS_sint64 written =
                PHYSFS_writeBytes(fh, json, (PHYSFS_uint64)len);
            PHYSFS_close(fh);
            PHYSFS_setWriteDir(prev_buf[0] ? prev_buf : NULL);
            cJSON_free(json);

            if ((size_t)written != len) {
                LOG_ERROR(LOG_TAG, "write error '%s' (physfs)", path);
                return false;
            }
            LOG_SUCCESS(LOG_TAG,
                "scene saved to '%s' (%zu bytes, physfs)", path, len);
            return true;
        }
        LOG_WARN(LOG_TAG,
            "PHYSFS_openWrite('%s') failed, falling back to fopen: %s",
            filename,
            PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
        PHYSFS_setWriteDir(prev_buf[0] ? prev_buf : NULL);
    } else if (PHYSFS_isInit()) {
        LOG_WARN(LOG_TAG,
            "PHYSFS_setWriteDir('%s') failed, falling back to fopen: %s",
            dir_buf,
            PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
    }

    /* Portable fallback using SDL I/O. */
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (!io) {
        cJSON_free(json);
        LOG_ERROR(LOG_TAG, "cannot open '%s' for writing", path);
        return false;
    }

    size_t written = SDL_WriteIO(io, json, len);
    SDL_CloseIO(io);
    cJSON_free(json);

    if (written != len) {
        LOG_ERROR(LOG_TAG, "write error '%s'", path);
        return false;
    }
    LOG_SUCCESS(LOG_TAG, "scene saved to '%s' (%zu bytes)", path, len);
    return true;
}

/* ── Load ─────────────────────────────────────────────────────────── */

bool jce_scene_serial_load(JceScene *scene, const char *json, size_t len)
{
    if (!scene || !json || len == 0) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        LOG_ERROR(LOG_TAG, "JSON parse error");
        return false;
    }

    /* Contract check (advisory: scene-layer parser ignores the envelope
     * and just walks entities, but a major-version mismatch should be
     * surfaced loudly). */
    uint32_t major = JCE_SCENE_CONTRACT_MAJOR;
    uint32_t minor = JCE_SCENE_CONTRACT_MINOR;
    const cJSON *contract = cJSON_GetObjectItemCaseSensitive(root,
        JCE_SCENE_CONTRACT_KEY);
    if (cJSON_IsObject(contract)) {
        const cJSON *mj = cJSON_GetObjectItemCaseSensitive(contract,
            JCE_SCENE_CONTRACT_MAJOR_KEY);
        const cJSON *mn = cJSON_GetObjectItemCaseSensitive(contract,
            JCE_SCENE_CONTRACT_MINOR_KEY);
        if (cJSON_IsNumber(mj)) major = (uint32_t)mj->valueint;
        if (cJSON_IsNumber(mn)) minor = (uint32_t)mn->valueint;
    }
    if (!jce_scene_contract_major_compatible(major)) {
        LOG_ERROR(LOG_TAG,
            "unsupported scene contract major version: %u (expected %u)",
            (unsigned)major, (unsigned)JCE_SCENE_CONTRACT_MAJOR);
        cJSON_Delete(root);
        return false;
    }
    (void)minor;

    int n = jce_scene_load_json(scene, root);
    cJSON_Delete(root);
    return n >= 0;
}

bool jce_scene_serial_load_file(JceScene *scene, const char *path)
{
    if (!scene || !path) return false;

    /* Try PhysFS first for cross-platform consistency. */
    if (PHYSFS_isInit()) {
        const char *sep = strrchr(path, '/');
        if (!sep) sep = strrchr(path, '\\');

        char dir_buf[1024];
        const char *filename = path;
        if (sep) {
            size_t dir_len = (size_t)(sep - path);
            if (dir_len >= sizeof(dir_buf)) dir_len = sizeof(dir_buf) - 1;
            memcpy(dir_buf, path, dir_len);
            dir_buf[dir_len] = '\0';
            filename = sep + 1;
        } else {
            dir_buf[0] = '.';
            dir_buf[1] = '\0';
        }

        if (PHYSFS_mount(dir_buf, NULL, 1)) {
            PHYSFS_File *fh = PHYSFS_openRead(filename);
            if (fh) {
                PHYSFS_sint64 sz = PHYSFS_fileLength(fh);
                if (sz > 0) {
                    char *buf = (char *)JCE_MALLOC((size_t)sz + 1);
                    if (buf) {
                        PHYSFS_sint64 nread =
                            PHYSFS_readBytes(fh, buf, (PHYSFS_uint64)sz);
                        PHYSFS_close(fh);
                        PHYSFS_unmount(dir_buf);

                        if (nread > 0) {
                            buf[nread] = '\0';
                            bool ok = jce_scene_serial_load(scene, buf,
                                (size_t)nread);
                            JCE_FREE(buf);
                            return ok;
                        }
                        JCE_FREE(buf);
                        return false;
                    }
                    PHYSFS_close(fh);
                }
            }
            PHYSFS_unmount(dir_buf);
        }
        LOG_DEBUG(LOG_TAG,
            "PhysFS load of '%s' failed, using fallback", path);
    }

    /* Fallback to SDL I/O when PhysFS unavailable or path not found. */
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) {
        LOG_ERROR(LOG_TAG, "cannot open '%s' for reading", path);
        return false;
    }

    Sint64 sz = SDL_GetIOSize(io);
    if (sz <= 0) { SDL_CloseIO(io); return false; }

    char *buf = (char *)JCE_MALLOC((size_t)sz + 1);
    if (!buf) { SDL_CloseIO(io); return false; }

    size_t read_bytes = SDL_ReadIO(io, buf, (size_t)sz);
    SDL_CloseIO(io);
    buf[read_bytes] = '\0';

    bool ok = jce_scene_serial_load(scene, buf, read_bytes);
    JCE_FREE(buf);
    return ok;
}

bool jce_scene_serial_load_vfs(JceScene *scene,
                               const JceFileSystem *fs,
                               const char *virtual_path)
{
    if (!scene || !fs || !virtual_path) return false;

    size_t size = 0;
    void *data = jce_fs_read_all(fs, virtual_path, &size);
    if (!data) {
        LOG_ERROR(LOG_TAG, "cannot open '%s' via VFS", virtual_path);
        return false;
    }

    char *buf = (char *)JCE_MALLOC(size + 1);
    if (!buf) { JCE_FREE(data); return false; }
    memcpy(buf, data, size);
    buf[size] = '\0';
    JCE_FREE(data);

    bool ok = jce_scene_serial_load(scene, buf, size);
    JCE_FREE(buf);
    return ok;
}

/* ── Memory ───────────────────────────────────────────────────────── */

void jce_scene_serial_free(char *json)
{
    /* cJSON_PrintUnformatted allocates with cJSON_malloc. */
    if (json) cJSON_free(json);
}
