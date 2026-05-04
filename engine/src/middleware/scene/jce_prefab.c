/*
 * jce_prefab.c  Prefab system implementation (P3-32).
 */

#include <jce/middleware/scene/jce_prefab.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_scene_serial.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "prefab"

/* ── Save ─────────────────────────────────────────────────────────── */

bool jce_prefab_save_subtree(const JceScene *scene, JceEntity root,
                              const char *path)
{
    if (!scene || root == 0 || !path) return false;

    JceJson *r = jce_scene_save_subtree_json(scene, root);
    if (!r) {
        LOG_ERROR(LOG_TAG, "subtree serialize failed for entity %llu",
                  (unsigned long long)root);
        return false;
    }

    char *json = jce_json_print(r, /*pretty=*/false);
    jce_json_free(r);
    if (!json) return false;

    size_t len = strlen(json);
    bool ok = jce_fs_host_write_all(path, json, len);
    jce_json_free_string(json);

    if (!ok) {
        LOG_ERROR(LOG_TAG, "cannot write prefab '%s'", path);
        return false;
    }
    LOG_SUCCESS(LOG_TAG, "prefab saved '%s' (%zu bytes)", path, len);
    return true;
}

/* ── Instantiate (shared internal) ───────────────────────────────── */

static JceEntity finalize_instance(JceScene       *scene,
                                    JceEntity      *new_ents,
                                    uint32_t        new_count,
                                    const char     *prefab_path,
                                    const jce_vec3 *position_offset)
{
    if (new_count == 0) {
        if (new_ents) jce_scene_serial_free_entities(new_ents);
        return 0;
    }

    JceEntity root = new_ents[0];

    /* Tag every newly-created entity with the prefab link.  Preserve
     * any pre-existing name/tag/colour the loader populated. */
    for (uint32_t i = 0; i < new_count; i++) {
        JceEntity      e = new_ents[i];
        JceEditorMeta *m = jce_scene_get_editor_meta(scene, e);
        JceEditorMeta  tmp;
        if (m) {
            tmp = *m;
        } else {
            memset(&tmp, 0, sizeof(tmp));
            tmp.enabled  = true;
            tmp.tag_color = 0;
        }
        tmp.prefab_instance = true;
        /* Truncating copy is fine — header-defined bound. */
        size_t cap = sizeof(tmp.prefab_path);
        strncpy(tmp.prefab_path, prefab_path, cap - 1);
        tmp.prefab_path[cap - 1] = '\0';
        jce_scene_set_editor_meta(scene, e, &tmp);
    }

    /* Optional translation on the root. */
    if (position_offset) {
        JceTransform *t = jce_scene_get_transform(scene, root);
        if (t) {
            t->position.x += position_offset->x;
            t->position.y += position_offset->y;
            t->position.z += position_offset->z;
        }
    }

    jce_scene_serial_free_entities(new_ents);
    return root;
}

/* ── Instantiate (VFS) ────────────────────────────────────────────── */

JceEntity jce_prefab_instantiate(JceScene             *scene,
                                  const JceFileSystem  *fs,
                                  const char           *virtual_path,
                                  const jce_vec3       *position_offset)
{
    if (!scene || !virtual_path) return 0;

    uint64_t size = 0;
    void  *buf  = NULL;

    if (fs) {
        buf = jce_fs_read_all(fs, virtual_path, &size);
    } else {
        buf = jce_fs_host_read_all(virtual_path, &size);
    }
    if (!buf || size == 0) {
        if (buf) JCE_FREE(buf);
        LOG_ERROR(LOG_TAG, "cannot read prefab '%s'", virtual_path);
        return 0;
    }

    JceEntity *new_ents  = NULL;
    uint32_t   new_count = 0;
    bool ok = jce_scene_serial_load_additive(scene,
                                              (const char *)buf, size,
                                              &new_ents, &new_count);
    JCE_FREE(buf);

    if (!ok) {
        LOG_ERROR(LOG_TAG, "additive load failed for '%s'", virtual_path);
        return 0;
    }
    return finalize_instance(scene, new_ents, new_count,
                             virtual_path, position_offset);
}

JceEntity jce_prefab_instantiate_file(JceScene       *scene,
                                       const char     *path,
                                       const jce_vec3 *position_offset)
{
    return jce_prefab_instantiate(scene, NULL, path, position_offset);
}

/* ── Queries ──────────────────────────────────────────────────────── */

typedef struct {
    const char *prefab_path;
    uint32_t    count;
    JceScene   *scene;
} CountCtx;

static void count_cb(JceScene *s, JceEntity e, void *ud)
{
    CountCtx *ctx = (CountCtx *)ud;
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    if (!m || !m->prefab_instance) return;
    if (strcmp(m->prefab_path, ctx->prefab_path) == 0)
        ctx->count++;
}

uint32_t jce_prefab_count_instances(const JceScene *scene,
                                      const char     *virtual_path)
{
    if (!scene || !virtual_path) return 0;
    CountCtx ctx = { virtual_path, 0, (JceScene *)scene };
    jce_scene_each_entity((JceScene *)scene, count_cb, &ctx);
    return ctx.count;
}
