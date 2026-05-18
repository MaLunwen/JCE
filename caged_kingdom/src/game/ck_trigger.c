/*
 * ck_trigger.c — see ck_trigger.h.
 *
 * Format read (all keys optional except `id`/`center`):
 *
 *   "ck_quest": {
 *     "questId": "act1_m01_wake",
 *     "objectives": [
 *       { "id": "leave_safehouse", "type": "trigger_zone",
 *         "center": [5.0, 0.0, 2.0], "radius": 2.5 }
 *     ]
 *   }
 *
 * Unknown objective `type` values are skipped (forwards compatibility
 * with future objective kinds, e.g. "kill_count", "interact").  The
 * scene's `ck_quest` block is silently absent in older scenes — those
 * yield an empty (but non-NULL) trigger set.
 */

#include "ck_trigger.h"

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include <stdint.h>
#include <string.h>

#define LOG_TAG "ck_trigger"

typedef struct CkTriggerZone {
    char  id[64];
    float center[3];
    float radius;
} CkTriggerZone;

struct CkTriggerSet {
    CkTriggerZone *zones;
    size_t         count;
    char          *quest_id;  /* may be NULL */
};

static void copy_id(char dst[64], const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= 64) n = 63;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

CkTriggerSet *ck_trigger_set_load_vfs(JceFileSystem *fs, const char *vfs_path)
{
    if (!fs || !vfs_path) {
        return NULL;
    }

    uint64_t size = 0;
    void *raw = jce_fs_read_all(fs, vfs_path, &size);
    if (!raw || size == 0) {
        LOG_ERROR(LOG_TAG, "cannot read '%s' via VFS", vfs_path);
        jce_free(raw);
        return NULL;
    }

    JceJson *root = jce_json_parse((const char *)raw, (size_t)size);
    jce_free(raw);
    if (!root || !jce_json_is_object(root)) {
        LOG_ERROR(LOG_TAG, "'%s' is not a JSON object", vfs_path);
        jce_json_free(root);
        return NULL;
    }

    CkTriggerSet *set = (CkTriggerSet *)jce_malloc(sizeof(*set));
    if (!set) {
        jce_json_free(root);
        return NULL;
    }
    memset(set, 0, sizeof(*set));

    JceJson *quest = jce_json_get(root, "ck_quest");
    if (!quest || !jce_json_is_object(quest)) {
        /* Scene has no ck_quest block — return an empty set. */
        jce_json_free(root);
        return set;
    }

    const char *qid = jce_json_get_string(quest, "questId", NULL);
    if (qid) {
        size_t n = strlen(qid);
        set->quest_id = (char *)jce_malloc(n + 1);
        if (set->quest_id) memcpy(set->quest_id, qid, n + 1);
    }

    JceJson *objs = jce_json_get(quest, "objectives");
    int n_objs = jce_json_is_array(objs) ? jce_json_array_size(objs) : 0;
    if (n_objs <= 0) {
        jce_json_free(root);
        return set;
    }

    set->zones = (CkTriggerZone *)jce_malloc((size_t)n_objs * sizeof(*set->zones));
    if (!set->zones) {
        jce_json_free(root);
        ck_trigger_set_destroy(set);
        return NULL;
    }

    for (int i = 0; i < n_objs; ++i) {
        JceJson *obj = jce_json_array_at(objs, i);
        if (!obj || !jce_json_is_object(obj)) continue;

        const char *type = jce_json_get_string(obj, "type", "trigger_zone");
        if (strcmp(type, "trigger_zone") != 0) {
            LOG_INFO(LOG_TAG, "skip objective '%s' of unsupported type '%s'",
                     jce_json_get_string(obj, "id", "?"), type);
            continue;
        }

        CkTriggerZone *z = &set->zones[set->count];
        copy_id(z->id, jce_json_get_string(obj, "id", ""));

        /* Accept either { "center": [x,y,z] } or scalar "centerX/Y/Z" */
        const float zero[3] = {0, 0, 0};
        JceJson *c = jce_json_get(obj, "center");
        if (jce_json_is_array(c) && jce_json_array_size(c) >= 3) {
            jce_json_get_floats(obj, "center", z->center, 3, zero);
        } else {
            jce_json_get_xyz(obj, "center", z->center, zero);
        }
        z->radius = (float)jce_json_get_number(obj, "radius", 1.0);
        set->count++;
    }

    jce_json_free(root);

    LOG_INFO(LOG_TAG, "loaded %zu zone(s) for quest '%s' from '%s'",
             set->count,
             set->quest_id ? set->quest_id : "<none>",
             vfs_path);
    return set;
}

void ck_trigger_set_destroy(CkTriggerSet *set)
{
    if (!set) return;
    jce_free(set->zones);
    jce_free(set->quest_id);
    jce_free(set);
}

const char *ck_trigger_set_quest_id(const CkTriggerSet *set)
{
    return set ? set->quest_id : NULL;
}

size_t ck_trigger_set_count(const CkTriggerSet *set)
{
    return set ? set->count : 0;
}

const char *ck_trigger_set_check(const CkTriggerSet *set, const float pos[3])
{
    if (!set || !pos || set->count == 0) return NULL;
    for (size_t i = 0; i < set->count; ++i) {
        const CkTriggerZone *z = &set->zones[i];
        float dx = pos[0] - z->center[0];
        float dz = pos[2] - z->center[2];
        if (dx * dx + dz * dz <= z->radius * z->radius) {
            return z->id;
        }
    }
    return NULL;
}
