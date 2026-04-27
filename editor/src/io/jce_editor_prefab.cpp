/*
 * jce_editor_prefab.cpp  Prefab lifecycle operations.
 *
 * Implements save, instantiate, revert, and query for prefab instances.
 * Reads/writes prefab metadata directly through the engine ECS EditorMeta
 * component — no editor mirror store.
 */

#include "jce_editor_file_util.h"
#include "core/jce_editor_state_internal.h"

/* ── Save prefab ─────────────────────────────────────────────────── */

bool jce_state_save_prefab(uint32_t entity_id, const char *prefab_path)
{
    if (!prefab_path || prefab_path[0] == '\0')
        return false;
    if (!jce_state_entity_exists(entity_id))
        return false;

    JceJson *root = build_prefab_json_root(entity_id);
    if (!root)
        return false;

    if (!ed_write_json_to_file(prefab_path, root)) {
        LOG_WARN(LOG_TAG, "prefab save failed: %s", prefab_path);
        return false;
    }

    HistoryEditScope edit_scope;
    mark_prefab_instance_recursive(entity_id, prefab_path);
    LOG_INFO(LOG_TAG, "prefab saved: %s", prefab_path);
    return true;
}

/* ── Instantiate prefab ──────────────────────────────────────────── */

uint32_t jce_state_instantiate_prefab(const char *prefab_path, uint32_t parent_id)
{
    if (!prefab_path || prefab_path[0] == '\0')
        return 0;

    size_t file_size = 0;
    char *buf = (char *)ed_read_file(prefab_path, &file_size);
    if (!buf) {
        LOG_WARN(LOG_TAG, "prefab instantiate failed, cannot read file: %s", prefab_path);
        return 0;
    }
    buf[file_size] = '\0';

    JceJson *root = jce_json_parse(buf, file_size);
    ED_FREE(buf);
    if (!root) {
        LOG_WARN(LOG_TAG, "prefab instantiate failed, JSON parse error: %s", prefab_path);
        return 0;
    }

    int contract_major = (int)JCE_SCENE_CONTRACT_MAJOR;
    int contract_minor = (int)JCE_SCENE_CONTRACT_MINOR;
    parse_scene_contract_version(root, &contract_major, &contract_minor);
    (void)contract_minor;
    if (!jce_scene_contract_major_compatible((uint32_t)contract_major)) {
        LOG_WARN(LOG_TAG,
                 "prefab instantiate failed, unsupported contract major %d: %s",
                 contract_major, prefab_path);
        jce_json_free(root);
        return 0;
    }

    const JceJson *node = find_prefab_root_node(root);
    if (!node) {
        jce_json_free(root);
        LOG_WARN(LOG_TAG, "prefab instantiate failed, missing root node: %s", prefab_path);
        return 0;
    }

    HistoryEditScope edit_scope;
    uint32_t id = load_entity_tree_node(node, parent_id);

    /* Variant: capture the parent path before we free the JSON tree. */
    const char *variant_parent =
        jce_json_get_string(root, "$variantOf", NULL);
    char vp_copy[260] = {0};
    if (variant_parent && variant_parent[0] != '\0') {
        snprintf(vp_copy, sizeof(vp_copy), "%s", variant_parent);
    }

    jce_json_free(root);

    if (id != 0) {
        mark_prefab_instance_recursive(id, prefab_path);
        if (vp_copy[0] != '\0') {
            JceEditorMeta *m =
                jce_scene_get_editor_meta(s.scene, (JceEntity)id);
            if (m) {
                snprintf(m->variant_parent_path,
                         sizeof(m->variant_parent_path),
                         "%s", vp_copy);
            }
        }
    }

    return id;
}

/* ── Revert prefab instance ──────────────────────────────────────── */

bool jce_state_revert_prefab(uint32_t entity_id)
{
    if (!jce_state_entity_exists(entity_id))
        return false;
    if (!jce_state_entity_is_prefab(entity_id))
        return false;

    const char *path = jce_state_entity_prefab_path(entity_id);
    if (!path || path[0] == '\0')
        return false;

    uint32_t parent_id = jce_state_entity_parent(entity_id);
    bool was_selected = jce_state_is_selected(entity_id);
    char prefab_path[JCE_MAX_PREFAB_PATH];
    snprintf(prefab_path, sizeof(prefab_path), "%s", path);

    HistoryEditScope edit_scope;
    uint32_t new_id = jce_state_instantiate_prefab(prefab_path, parent_id);
    if (new_id == 0)
        return false;

    jce_state_delete_entity(entity_id);

    if (was_selected)
        jce_state_select_entity(new_id, false);
    return true;
}

/* ── Prefab queries ──────────────────────────────────────────────── */

bool jce_state_is_prefab_instance(uint32_t entity_id)
{
    return jce_state_entity_is_prefab(entity_id);
}

const char *jce_state_get_prefab_path(uint32_t entity_id)
{
    if (!jce_state_entity_is_prefab(entity_id))
        return NULL;
    const char *p = jce_state_entity_prefab_path(entity_id);
    return (p && p[0] != '\0') ? p : NULL;
}

/* ── Prefab Variant (P1 #10, MVP format-only) ──────────────────────
 *
 * On disk a Variant looks identical to a regular prefab plus a
 * top-level `$variantOf` string pointing at the parent prefab's
 * project-relative path:
 *
 *     {
 *       "$variantOf": "Assets/Prefabs/Enemy.prefab",
 *       "@contract": { ... },
 *       "prefab":    { "version": 1, "root": { ... full snapshot ... } }
 *     }
 *
 * v0.7.12 stores the full snapshot (no diff vs parent yet) so the
 * loader can be a one-line check that ignores `$variantOf` for
 * runtime purposes — this gives us the file-format and UI surface
 * without committing to a serializer-wide override-tracking system.
 * Subsequent commits will replace the snapshot with a true diff. */

bool jce_state_save_prefab_variant(uint32_t entity_id,
                                   const char *variant_path,
                                   const char *parent_prefab_path)
{
    if (!variant_path || variant_path[0] == '\0') return false;
    if (!parent_prefab_path || parent_prefab_path[0] == '\0') return false;
    if (!jce_state_entity_exists(entity_id)) return false;

    JceJson *root = build_prefab_json_root(entity_id);
    if (!root) return false;

    jce_json_set_string(root, "$variantOf", parent_prefab_path);

    if (!ed_write_json_to_file(variant_path, root)) {
        LOG_WARN(LOG_TAG, "prefab variant save failed: %s", variant_path);
        return false;
    }

    HistoryEditScope edit_scope;
    mark_prefab_instance_recursive(entity_id, variant_path);
    {
        JceEditorMeta *m =
            jce_scene_get_editor_meta(s.scene, (JceEntity)entity_id);
        if (m) {
            snprintf(m->variant_parent_path,
                     sizeof(m->variant_parent_path),
                     "%s", parent_prefab_path);
        }
    }
    LOG_INFO(LOG_TAG, "prefab variant saved: %s (parent=%s)",
             variant_path, parent_prefab_path);
    return true;
}

const char *jce_state_get_variant_parent(uint32_t entity_id)
{
    if (!s.scene || entity_id == 0) return NULL;
    JceEditorMeta *m =
        jce_scene_get_editor_meta(s.scene, (JceEntity)entity_id);
    if (!m || m->variant_parent_path[0] == '\0') return NULL;
    return m->variant_parent_path;
}
