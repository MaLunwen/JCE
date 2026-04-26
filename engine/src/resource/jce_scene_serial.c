/*
 * jce_scene_serial.c  Resource-layer scene I/O (thin wrapper).
 *
 * Single source of truth for component (de)serialization lives in the
 * scene layer (engine/src/scene/jce_scene_serial.c). This module only
 * adds:
 *   1. Host-path file I/O via jce_fs_host_read_all/write_all (SDL backend).
 *   2. VFS-aware load via JceFileSystem (PhysFS backend, asset runtime).
 *   3. The legacy `jce_scene_serial_*` C API still consumed by the
 *      editor's history/snapshot system and by ck.
 *
 * Dogfooding: editor saves and ck loads through the same JSON parser,
 * so the on-disk format can never diverge between tools.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h> /* canonical (de)serializer */
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_scene_contract.h>
#include <jce/resource/jce_scene_serial.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "scene_serial"

/* ── Save ─────────────────────────────────────────────────────────── */

char *jce_scene_serial_save(const JceScene *scene, size_t *out_len)
{
    if (!scene) return NULL;

    JceJson *root = jce_scene_save_json(scene);
    if (!root) return NULL;

    char *json = jce_json_print(root, /*pretty=*/false);
    jce_json_free(root);

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

    bool ok = jce_fs_host_write_all(path, json, len);
    jce_json_free_string(json);

    if (!ok) {
        LOG_ERROR(LOG_TAG, "cannot write '%s'", path);
        return false;
    }
    LOG_SUCCESS(LOG_TAG, "scene saved to '%s' (%zu bytes)", path, len);
    return true;
}

/* ── Load ─────────────────────────────────────────────────────────── */

bool jce_scene_serial_load(JceScene *scene, const char *json, size_t len)
{
    if (!scene || !json || len == 0) return false;

    JceJson *root = jce_json_parse(json, len);
    if (!root) {
        LOG_ERROR(LOG_TAG, "JSON parse error");
        return false;
    }

    /* Contract check (advisory). */
    uint32_t major = JCE_SCENE_CONTRACT_MAJOR;
    uint32_t minor = JCE_SCENE_CONTRACT_MINOR;
    const JceJson *contract = jce_json_get(root, JCE_SCENE_CONTRACT_KEY);
    if (jce_json_is_object(contract)) {
        major = (uint32_t)jce_json_get_int(contract,
            JCE_SCENE_CONTRACT_MAJOR_KEY, (int)major);
        minor = (uint32_t)jce_json_get_int(contract,
            JCE_SCENE_CONTRACT_MINOR_KEY, (int)minor);
    }
    if (!jce_scene_contract_major_compatible(major)) {
        LOG_ERROR(LOG_TAG,
            "unsupported scene contract major version: %u (expected %u)",
            (unsigned)major, (unsigned)JCE_SCENE_CONTRACT_MAJOR);
        jce_json_free(root);
        return false;
    }
    (void)minor;

    int n = jce_scene_load_json(scene, root);
    jce_json_free(root);
    return n >= 0;
}

bool jce_scene_serial_load_file(JceScene *scene, const char *path)
{
    if (!scene || !path) return false;

    /* Derive base directory from the path and inform the parser, so
       sibling material backfill (Unity-style) can resolve. */
    {
        const char *sep = strrchr(path, '/');
        const char *bs  = strrchr(path, '\\');
        if (bs > sep) sep = bs;
        if (sep) {
            char dir[1024];
            size_t L = (size_t)(sep - path);
            if (L >= sizeof(dir)) L = sizeof(dir) - 1;
            memcpy(dir, path, L);
            dir[L] = '\0';
            jce_scene_serial_set_base_dir(dir);
        } else {
            jce_scene_serial_set_base_dir(NULL);
        }
    }

    /* Scene file dialogs operate on real OS paths -> host filesystem. */
    size_t size = 0;
    char *buf = (char *)jce_fs_host_read_all(path, &size);
    if (!buf) {
        LOG_ERROR(LOG_TAG, "cannot open '%s' for reading", path);
        return false;
    }

    bool ok = jce_scene_serial_load(scene, buf, size);
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
    jce_json_free_string(json);
}
