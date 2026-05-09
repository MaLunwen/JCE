/*
 * jce_prefab_overrides.c  Sparse override-patch storage with JSON sidecar.
 *
 * Storage is a flat array of (entity_path, component_id, field_path,
 * value_json) tuples.  Lookups for set/clear use linear scan — at
 * O(N) for ≤ 64 typical overrides this is faster than a hash table
 * once string-key hashing is accounted for.  Capacity caps at 1024 to
 * detect runaway tracking; raise if real projects need more.
 *
 * String memory is owned by the set: each entry strdup's its three
 * string fields and free's them on clear/destroy.  The iteration API
 * returns interior pointers that remain valid until the next mutation.
 */

#include <jce/middleware/scene/jce_prefab_overrides.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#define LOG_TAG       "prefab_ov"
#define MAX_OVERRIDES 1024

typedef struct {
    char    *entity_path;     /* strdup-owned */
    uint64_t component_id;
    char    *field_path;
    char    *value_json;
} Entry;

struct JcePrefabOverrideSet {
    Entry    entries[MAX_OVERRIDES];
    uint32_t count;
};

/* ── Internal helpers ─────────────────────────────────────────────── */

static char *str_dup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *d = (char *)JCE_MALLOC(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n + 1);
    return d;
}

static void entry_free(Entry *e)
{
    JCE_FREE(e->entity_path);
    JCE_FREE(e->field_path);
    JCE_FREE(e->value_json);
    e->entity_path = e->field_path = e->value_json = NULL;
    e->component_id = 0;
}

static int find_entry(const JcePrefabOverrideSet *set,
                      const char *entity_path, uint64_t component_id,
                      const char *field_path)
{
    for (uint32_t i = 0; i < set->count; ++i) {
        const Entry *e = &set->entries[i];
        if (e->component_id != component_id) continue;
        if (strcmp(e->entity_path ? e->entity_path : "", entity_path) != 0) continue;
        if (strcmp(e->field_path  ? e->field_path  : "", field_path)  != 0) continue;
        return (int)i;
    }
    return -1;
}

/* Map a component_id bit to a stable JSON string for serialisation.
 * Only the most common types — extend as the override system gets
 * exercised on more component kinds.  Unknown ids serialise as
 * "comp_<hex>". */
static const char *component_id_to_string(uint64_t id)
{
    switch (id) {
        case (UINT64_C(1) << 0):  return "transform";
        case (UINT64_C(1) << 1):  return "meshRenderer";
        case (UINT64_C(1) << 2):  return "camera";
        case (UINT64_C(1) << 3):  return "directionalLight";
        case (UINT64_C(1) << 4):  return "pointLight";
        case (UINT64_C(1) << 5):  return "spotLight";
        case (UINT64_C(1) << 21): return "editorMeta";
        default: return NULL;
    }
}

static uint64_t component_string_to_id(const char *name)
{
    if (!name || !name[0]) return 0;
    if (strcmp(name, "transform") == 0)        return (UINT64_C(1) << 0);
    if (strcmp(name, "meshRenderer") == 0)     return (UINT64_C(1) << 1);
    if (strcmp(name, "camera") == 0)           return (UINT64_C(1) << 2);
    if (strcmp(name, "directionalLight") == 0) return (UINT64_C(1) << 3);
    if (strcmp(name, "pointLight") == 0)       return (UINT64_C(1) << 4);
    if (strcmp(name, "spotLight") == 0)        return (UINT64_C(1) << 5);
    if (strcmp(name, "editorMeta") == 0)       return (UINT64_C(1) << 21);
    /* Hex fallback "comp_<hex>". */
    if (strncmp(name, "comp_", 5) == 0) {
        return (uint64_t)strtoull(name + 5, NULL, 16);
    }
    return 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

JcePrefabOverrideSet *jce_prefab_overrides_create(void)
{
    JcePrefabOverrideSet *s = (JcePrefabOverrideSet *)JCE_CALLOC(1, sizeof(*s));
    return s;
}

void jce_prefab_overrides_destroy(JcePrefabOverrideSet *s)
{
    if (!s) return;
    for (uint32_t i = 0; i < s->count; ++i) entry_free(&s->entries[i]);
    JCE_FREE(s);
}

/* ── Mutate ───────────────────────────────────────────────────────── */

bool jce_prefab_overrides_set(JcePrefabOverrideSet *s,
                              const char *entity_path,
                              uint64_t component_id,
                              const char *field_path,
                              const char *value_json)
{
    if (!s || !field_path || !value_json) return false;
    if (!entity_path) entity_path = "";

    int existing = find_entry(s, entity_path, component_id, field_path);
    if (existing >= 0) {
        Entry *e = &s->entries[existing];
        char *new_val = str_dup(value_json);
        if (!new_val) return false;
        JCE_FREE(e->value_json);
        e->value_json = new_val;
        return true;
    }

    if (s->count >= MAX_OVERRIDES) {
        LOG_ERROR(LOG_TAG, "override capacity exhausted (%d)", MAX_OVERRIDES);
        return false;
    }
    Entry *e = &s->entries[s->count];
    e->entity_path  = str_dup(entity_path);
    e->field_path   = str_dup(field_path);
    e->value_json   = str_dup(value_json);
    e->component_id = component_id;
    if (!e->entity_path || !e->field_path || !e->value_json) {
        entry_free(e);
        return false;
    }
    s->count++;
    return true;
}

bool jce_prefab_overrides_clear_one(JcePrefabOverrideSet *s,
                                    const char *entity_path,
                                    uint64_t component_id,
                                    const char *field_path)
{
    if (!s || !field_path) return false;
    if (!entity_path) entity_path = "";
    int idx = find_entry(s, entity_path, component_id, field_path);
    if (idx < 0) return false;
    entry_free(&s->entries[idx]);
    /* Compact: swap last into the hole. */
    if ((uint32_t)idx + 1 < s->count) {
        s->entries[idx] = s->entries[s->count - 1];
        memset(&s->entries[s->count - 1], 0, sizeof(Entry));
    }
    s->count--;
    return true;
}

void jce_prefab_overrides_clear_all(JcePrefabOverrideSet *s)
{
    if (!s) return;
    for (uint32_t i = 0; i < s->count; ++i) entry_free(&s->entries[i]);
    s->count = 0;
}

/* ── Iteration ────────────────────────────────────────────────────── */

uint32_t jce_prefab_overrides_count(const JcePrefabOverrideSet *s)
{
    return s ? s->count : 0u;
}

JcePrefabOverride jce_prefab_overrides_get_at(const JcePrefabOverrideSet *s,
                                                uint32_t index)
{
    JcePrefabOverride out = { "", 0, "", "" };
    if (!s || index >= s->count) return out;
    const Entry *e = &s->entries[index];
    out.entity_path  = e->entity_path  ? e->entity_path  : "";
    out.component_id = e->component_id;
    out.field_path   = e->field_path   ? e->field_path   : "";
    out.value_json   = e->value_json   ? e->value_json   : "";
    return out;
}

/* ── Persistence ──────────────────────────────────────────────────── */

bool jce_prefab_overrides_save_to_file(const JcePrefabOverrideSet *s,
                                        const char *path)
{
    if (!s || !path) return false;

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "overrides");
    if (!arr) { cJSON_Delete(root); return false; }

    for (uint32_t i = 0; i < s->count; ++i) {
        const Entry *e = &s->entries[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", e->entity_path ? e->entity_path : "");
        const char *cn = component_id_to_string(e->component_id);
        if (cn) {
            cJSON_AddStringToObject(o, "component", cn);
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "comp_%llx",
                     (unsigned long long)e->component_id);
            cJSON_AddStringToObject(o, "component", buf);
        }
        cJSON_AddStringToObject(o, "field", e->field_path ? e->field_path : "");
        /* value_json is already a JSON literal — parse it so the saved
         * file uses the natural type rather than a string-of-JSON. */
        cJSON *parsed = cJSON_Parse(e->value_json ? e->value_json : "null");
        if (parsed) {
            cJSON_AddItemToObject(o, "value", parsed);
        } else {
            cJSON_AddStringToObject(o, "value", e->value_json ? e->value_json : "");
        }
        cJSON_AddItemToArray(arr, o);
    }

    char *txt = cJSON_PrintBuffered(root, 4096, 1);
    cJSON_Delete(root);
    if (!txt) return false;

    bool ok = jce_fs_host_write_all(path, txt, strlen(txt));
    free(txt);
    return ok;
}

/* ── Apply / Revert helpers ───────────────────────────────────────── */

uint32_t jce_prefab_overrides_count_for_entity(const JcePrefabOverrideSet *s,
                                                const char *entity_path)
{
    if (!s) return 0;
    if (!entity_path) entity_path = "";
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->count; ++i) {
        const Entry *e = &s->entries[i];
        if (strcmp(e->entity_path ? e->entity_path : "", entity_path) == 0)
            n++;
    }
    return n;
}

bool jce_prefab_overrides_has_any(const JcePrefabOverrideSet *s)
{
    return s && s->count > 0u;
}

/* Compact step: swap-with-last.  Returns the new count after removal. */
static void compact_remove_at(JcePrefabOverrideSet *s, uint32_t idx)
{
    entry_free(&s->entries[idx]);
    if (idx + 1u < s->count) {
        s->entries[idx] = s->entries[s->count - 1];
        memset(&s->entries[s->count - 1], 0, sizeof(Entry));
    }
    s->count--;
}

uint32_t jce_prefab_overrides_revert_entity(JcePrefabOverrideSet *s,
                                             const char *entity_path)
{
    if (!s) return 0;
    if (!entity_path) entity_path = "";
    uint32_t removed = 0;
    /* Iterate backwards so swap-with-last doesn't break the cursor. */
    for (uint32_t i = s->count; i-- > 0u; ) {
        const Entry *e = &s->entries[i];
        if (strcmp(e->entity_path ? e->entity_path : "", entity_path) == 0) {
            compact_remove_at(s, i);
            removed++;
        }
    }
    return removed;
}

uint32_t jce_prefab_overrides_revert_component(JcePrefabOverrideSet *s,
                                                const char *entity_path,
                                                uint64_t component_id)
{
    if (!s) return 0;
    if (!entity_path) entity_path = "";
    uint32_t removed = 0;
    for (uint32_t i = s->count; i-- > 0u; ) {
        const Entry *e = &s->entries[i];
        if (e->component_id != component_id) continue;
        if (strcmp(e->entity_path ? e->entity_path : "", entity_path) != 0) continue;
        compact_remove_at(s, i);
        removed++;
    }
    return removed;
}

/* Apply: load base prefab JSON, walk every override, mutate the JSON
 * tree in place, and write it back.  This is intentionally surgical —
 * we only touch entities/components/fields the override mentions, so
 * formatting and comments in untouched parts of the file are
 * preserved (cJSON re-emits but at least field order is stable).
 *
 * Implementation strategy:
 *   1. Parse base file → cJSON tree.
 *   2. For each override, navigate to the right entity by walking the
 *      "entities" array using EditorMeta.name slash-separated path.
 *   3. Find or insert the matching component object in that entity's
 *      "components" array (component_id → string mapping).
 *   4. Set `field_path` (dot-delimited; "position.x" splits) on the
 *      component, parsing value_json into a cJSON node.
 *   5. Re-emit and write back.
 *   6. On success, clear all overrides (they're now baked in).
 */

/* Walk a slash-path against the scene's entity tree and return the
 * matching entity cJSON object (or NULL).  Empty path returns the
 * first entity (treated as root). */
static cJSON *find_entity_by_path(cJSON *entities_arr, const char *path)
{
    if (!entities_arr || !cJSON_IsArray(entities_arr)) return NULL;
    if (!path || !path[0]) {
        return cJSON_GetArrayItem(entities_arr, 0);
    }
    /* For non-root, match against EditorMeta.name in any depth. */
    cJSON *e = NULL;
    cJSON_ArrayForEach(e, entities_arr) {
        const cJSON *meta_arr = cJSON_GetObjectItemCaseSensitive(e, "components");
        if (!cJSON_IsArray(meta_arr)) continue;
        const cJSON *c = NULL;
        cJSON_ArrayForEach(c, meta_arr) {
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(c, "type");
            if (!cJSON_IsString(type)) continue;
            if (strcmp(type->valuestring, "editorMeta") != 0) continue;
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(c, "name");
            if (cJSON_IsString(name) && strcmp(name->valuestring, path) == 0)
                return e;
        }
    }
    return NULL;
}

/* Find or create a component object of the given string type within an
 * entity's `components` array. */
static cJSON *find_or_add_component(cJSON *entity_obj, const char *type)
{
    if (!entity_obj || !type) return NULL;
    cJSON *comps = cJSON_GetObjectItemCaseSensitive(entity_obj, "components");
    if (!cJSON_IsArray(comps)) {
        comps = cJSON_AddArrayToObject(entity_obj, "components");
    }
    cJSON *c = NULL;
    cJSON_ArrayForEach(c, comps) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(c, "type");
        if (cJSON_IsString(t) && strcmp(t->valuestring, type) == 0) return c;
    }
    cJSON *fresh = cJSON_CreateObject();
    cJSON_AddStringToObject(fresh, "type", type);
    cJSON_AddItemToArray(comps, fresh);
    return fresh;
}

/* Set a dot-delimited field path on `obj` to `value` (taking ownership
 * of value).  Intermediate nodes are auto-created as objects. */
static void set_field_path(cJSON *obj, const char *field_path, cJSON *value)
{
    if (!obj || !field_path || !value) {
        if (value) cJSON_Delete(value);
        return;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", field_path);

    cJSON *cur = obj;
    char *save = NULL;
    char *tok = strtok_r(buf, ".", &save);
    char *next = tok ? strtok_r(NULL, ".", &save) : NULL;
    while (tok) {
        if (!next) {
            /* Leaf — replace existing or add new. */
            cJSON_DeleteItemFromObject(cur, tok);
            cJSON_AddItemToObject(cur, tok, value);
            return;
        }
        cJSON *child = cJSON_GetObjectItemCaseSensitive(cur, tok);
        if (!child || !cJSON_IsObject(child)) {
            cJSON_DeleteItemFromObject(cur, tok);
            child = cJSON_CreateObject();
            cJSON_AddItemToObject(cur, tok, child);
        }
        cur = child;
        tok = next;
        next = strtok_r(NULL, ".", &save);
    }
    /* Path was empty — fall through; release the unattached value. */
    cJSON_Delete(value);
}

bool jce_prefab_overrides_apply_to_base(JcePrefabOverrideSet *s,
                                         const char *base_prefab_path)
{
    if (!s || !base_prefab_path) return false;

    uint64_t size = 0;
    void *buf = jce_fs_host_read_all(base_prefab_path, &size);
    if (!buf || size == 0) {
        if (buf) jce_fs_buffer_free(buf);
        LOG_ERROR(LOG_TAG, "apply: cannot read base prefab '%s'", base_prefab_path);
        return false;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)buf, (size_t)size);
    jce_fs_buffer_free(buf);
    if (!root) {
        LOG_ERROR(LOG_TAG, "apply: invalid JSON in '%s'", base_prefab_path);
        return false;
    }

    cJSON *entities = cJSON_GetObjectItemCaseSensitive(root, "entities");
    if (!cJSON_IsArray(entities)) {
        cJSON_Delete(root);
        LOG_ERROR(LOG_TAG, "apply: prefab missing 'entities' array");
        return false;
    }

    uint32_t applied = 0;
    for (uint32_t i = 0; i < s->count; ++i) {
        const Entry *e = &s->entries[i];
        cJSON *entity = find_entity_by_path(entities,
                                            e->entity_path ? e->entity_path : "");
        if (!entity) continue;

        const char *type = component_id_to_string(e->component_id);
        if (!type) continue; /* Unknown component id — skip. */
        cJSON *comp = find_or_add_component(entity, type);
        if (!comp) continue;

        cJSON *value = cJSON_Parse(e->value_json ? e->value_json : "null");
        if (!value) continue;
        set_field_path(comp, e->field_path ? e->field_path : "", value);
        applied++;
    }

    char *txt = cJSON_Print(root);
    cJSON_Delete(root);
    if (!txt) return false;

    bool ok = jce_fs_host_write_all(base_prefab_path, txt, strlen(txt));
    free(txt);
    if (ok) {
        LOG_INFO(LOG_TAG, "apply: %u overrides → %s",
                 (unsigned)applied, base_prefab_path);
        jce_prefab_overrides_clear_all(s);
    }
    return ok;
}

bool jce_prefab_overrides_load_from_file(JcePrefabOverrideSet *s,
                                          const char *path)
{
    if (!s || !path) return false;

    uint64_t size = 0;
    void *buf = jce_fs_host_read_all(path, &size);
    if (!buf || size == 0) {
        if (buf) jce_fs_buffer_free(buf);
        return false;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)buf, (size_t)size);
    jce_fs_buffer_free(buf);
    if (!root) return false;

    jce_prefab_overrides_clear_all(s);

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "overrides");
    if (cJSON_IsArray(arr)) {
        cJSON *o = NULL;
        cJSON_ArrayForEach(o, arr) {
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "path");
            const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, "component");
            const cJSON *f = cJSON_GetObjectItemCaseSensitive(o, "field");
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "value");
            if (!cJSON_IsString(c) || !cJSON_IsString(f)) continue;
            const char *path_s  = cJSON_IsString(p) ? p->valuestring : "";
            uint64_t    cid     = component_string_to_id(c->valuestring);
            char       *value_s = v ? cJSON_PrintUnformatted(v) : NULL;
            jce_prefab_overrides_set(s, path_s, cid, f->valuestring,
                                      value_s ? value_s : "null");
            free(value_s);
        }
    }

    cJSON_Delete(root);
    return true;
}
