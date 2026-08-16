/*
 * jce_editor_prefab.cpp  Prefab lifecycle operations.
 *
 * Implements save, instantiate, revert, and query for prefab instances.
 * Reads/writes prefab metadata directly through the engine ECS EditorMeta
 * component — no editor mirror store.
 */

#include "jce_editor_file_util.h"
#include "core/jce_editor_state_internal.h"
#include "io/jce_editor_prefab_override.h"

extern "C" {
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
}

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
                m->variant_parent_path = jce_scene_intern(s.scene, vp_copy);
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
            m->variant_parent_path =
                jce_scene_intern(s.scene, parent_prefab_path);
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

/* ── Per-component prefab override Apply / Revert (Phase override) ─────
 *
 * Unity/Godot per-component workflow built on the byte-diff override core
 * (jce_editor_prefab_override).  Apply pushes ONE component from the
 * instance back to its source .prefab.json and updates every sibling
 * instance that was NOT overriding that component; Revert copies the
 * component from the source back onto the instance (so it stops differing
 * and drops out of the override set on the next save).  Both wrap their
 * scene mutations in begin/end_batch_edit for one atomic undo entry. */

namespace {

/* Load a prefab source root entity into a scratch scene (root-level
 * components only).  Caller owns the returned scene (jce_scene_destroy). */
JceScene *load_source_root(const char *prefab_path, JceEntity *out_root)
{
    if (out_root) *out_root = 0;
    if (!prefab_path || !prefab_path[0]) return NULL;

    JceJson *root_json = jce_json_parse_file(prefab_path);
    if (!root_json) return NULL;
    const JceJson *node = find_prefab_root_node(root_json);
    if (!node) { jce_json_free(root_json); return NULL; }

    JceScene *scratch = jce_scene_create();
    if (!scratch) { jce_json_free(root_json); return NULL; }
    JceEntity e = jce_scene_create_entity(scratch, "prefab_source");
    if (e == 0) {
        jce_scene_destroy(scratch);
        jce_json_free(root_json);
        return NULL;
    }
    jce_scene_parse_entity_json(scratch, e, node);
    jce_json_free(root_json);
    if (out_root) *out_root = e;
    return scratch;
}

/* Copy one component (by canonical name) FROM (src_scene,src_e) onto
 * (dst_scene,dst_e) via the proven serialize→parse round-trip, so all
 * cross-entity ref fixups and per-field parsing run.  Returns true when
 * the component was found + applied. */
bool copy_component_between(JceScene *dst_scene, JceEntity dst_e,
                            JceScene *src_scene, JceEntity src_e,
                            const char *comp_name)
{
    JceJson *node = jce_prefab_override::make_single_component_node(
        src_scene, src_e, comp_name);
    if (!node) return false;
    jce_scene_parse_entity_json(dst_scene, dst_e, node);
    jce_json_free(node);
    return true;
}

} /* anonymous namespace */

bool jce_state_apply_prefab_component(uint32_t entity_id,
                                      const char *comp_name)
{
    if (!comp_name || !comp_name[0]) return false;
    if (!jce_state_entity_exists(entity_id)) return false;
    if (!jce_state_entity_is_prefab(entity_id)) return false;

    const char *path = jce_state_entity_prefab_path(entity_id);
    if (!path || path[0] == '\0') return false;
    char prefab_path[JCE_MAX_PREFAB_PATH];
    snprintf(prefab_path, sizeof(prefab_path), "%s", path);

    JceEntity inst_e = jce_state_to_ecs_entity(entity_id);
    if (inst_e == 0) return false;

    /* Snapshot the OLD source (before mutation) so we can tell which
     * siblings were merely INHERITING this component (== old source) vs
     * carrying their own override (kept untouched). */
    JceEntity old_src_e = 0;
    JceScene *old_src = load_source_root(prefab_path, &old_src_e);
    if (!old_src) {
        LOG_WARN(LOG_TAG, "prefab apply-component: cannot load source %s",
                 prefab_path);
        return false;
    }

    /* ── 1. Write the component into the source .prefab.json ──────────
     * Rebuild the root node's "components" array: keep every entry whose
     * "type" differs from comp_name, then append the instance's current
     * serialized component.  (The JSON facade has no array replace, so we
     * detach kept entries via print+reparse.) */
    JceJson *root_json = jce_json_parse_file(prefab_path);
    if (!root_json) { jce_scene_destroy(old_src); return false; }
    JceJson *root_node = (JceJson *)find_prefab_root_node(root_json);
    if (!root_node) {
        jce_json_free(root_json);
        jce_scene_destroy(old_src);
        return false;
    }

    JceJson *new_comp = jce_prefab_override::make_single_component_node(
        s.scene, inst_e, comp_name);
    /* make_single_component_node wraps in { "components":[c] }; pull the
     * single child out for splicing into the source array. */
    JceJson *new_comp_obj = NULL;
    if (new_comp) {
        JceJson *arr = jce_json_get(new_comp, "components");
        if (jce_json_is_array(arr))
            new_comp_obj = jce_json_first_child(arr);
    }

    JceJson *old_comps = jce_json_get(root_node, "components");
    JceJson *rebuilt = jce_json_array();
    if (rebuilt && jce_json_is_array(old_comps)) {
        for (JceJson *c = jce_json_first_child(old_comps); c;
             c = jce_json_next_sibling(c)) {
            const char *type = jce_json_get_string(c, "type", NULL);
            if (type && strcmp(type, comp_name) == 0) continue; /* replaced */
            char *txt = jce_json_print(c, false);
            if (!txt) continue;
            JceJson *copy = jce_json_parse(txt, 0);
            jce_json_free_string(txt);
            if (copy) jce_json_array_push(rebuilt, copy);
        }
    }
    if (rebuilt && new_comp_obj) {
        char *txt = jce_json_print(new_comp_obj, false);
        if (txt) {
            JceJson *copy = jce_json_parse(txt, 0);
            jce_json_free_string(txt);
            if (copy) jce_json_array_push(rebuilt, copy);
        }
    }
    jce_json_free(new_comp);

    if (rebuilt) {
        jce_json_remove(root_node, "components");
        jce_json_set_child(root_node, "components", rebuilt);
    }

    bool write_ok = ed_write_json_to_file(prefab_path, root_json);
    /* ed_write_json_to_file took ownership of root_json. */
    if (!write_ok) {
        LOG_WARN(LOG_TAG, "prefab apply-component: write failed %s",
                 prefab_path);
        jce_scene_destroy(old_src);
        return false;
    }

    /* ── 2. Propagate to sibling instances (atomic undo) ─────────────
     * A sibling INHERITS this component when its bytes equal the OLD
     * source (i.e. it has no override of its own).  Update only those to
     * the new value; siblings overriding the component keep their value. */
    jce_state_begin_batch_edit();
    int updated = 0;
    int total = jce_state_get_entity_count();
    for (int i = 0; i < total; i++) {
        uint32_t sib = jce_state_get_entity_id_by_index(i);
        if (!sib || sib == entity_id) continue;
        if (!jce_state_entity_is_prefab(sib)) continue;
        const char *sp = jce_state_entity_prefab_path(sib);
        if (!sp || strcmp(sp, prefab_path) != 0) continue;

        JceEntity sib_e = jce_state_to_ecs_entity(sib);
        if (sib_e == 0) continue;
        /* Inheriting iff NOT overridden vs the OLD source. */
        if (jce_prefab_override::is_component_overridden(
                s.scene, sib_e, old_src, old_src_e, comp_name))
            continue;
        if (copy_component_between(s.scene, sib_e, s.scene, inst_e, comp_name))
            updated++;
    }
    jce_state_end_batch_edit();

    jce_scene_destroy(old_src);
    LOG_INFO(LOG_TAG,
             "prefab apply-component '%s' -> %s (%d sibling(s) updated)",
             comp_name, prefab_path, updated);
    return true;
}

bool jce_state_revert_prefab_component(uint32_t entity_id,
                                       const char *comp_name)
{
    if (!comp_name || !comp_name[0]) return false;
    if (!jce_state_entity_exists(entity_id)) return false;
    if (!jce_state_entity_is_prefab(entity_id)) return false;

    const char *path = jce_state_entity_prefab_path(entity_id);
    if (!path || path[0] == '\0') return false;
    char prefab_path[JCE_MAX_PREFAB_PATH];
    snprintf(prefab_path, sizeof(prefab_path), "%s", path);

    JceEntity inst_e = jce_state_to_ecs_entity(entity_id);
    if (inst_e == 0) return false;

    JceEntity src_e = 0;
    JceScene *src = load_source_root(prefab_path, &src_e);
    if (!src) {
        LOG_WARN(LOG_TAG, "prefab revert-component: cannot load source %s",
                 prefab_path);
        return false;
    }

    /* Copy the component from source back onto the instance — it then
     * matches the source byte-for-byte and drops out of the override set
     * on the next save (the override is computed, not stored). */
    jce_state_begin_batch_edit();
    bool ok = copy_component_between(s.scene, inst_e, src, src_e, comp_name);
    jce_state_end_batch_edit();

    jce_scene_destroy(src);
    if (ok)
        LOG_INFO(LOG_TAG, "prefab revert-component '%s' from %s",
                 comp_name, prefab_path);
    return ok;
}

/* Batch query: fill `out` with the canonical names of every component the
 * instance overrides vs its source (one source load).  Returns the count,
 * or -1 when the entity is not an instance / the source cannot load.  Far
 * cheaper than per-component is_overridden when the inspector needs the
 * whole set for a frame. */
int jce_state_get_prefab_overrides(uint32_t entity_id,
                                   char names[][64], int max_names)
{
    if (!jce_state_entity_exists(entity_id)) return -1;
    if (!jce_state_entity_is_prefab(entity_id)) return -1;
    const char *path = jce_state_entity_prefab_path(entity_id);
    if (!path || path[0] == '\0') return -1;

    JceEntity inst_e = jce_state_to_ecs_entity(entity_id);
    if (inst_e == 0) return -1;

    JceEntity src_e = 0;
    JceScene *src = load_source_root(path, &src_e);
    if (!src) return -1;

    std::vector<std::string> ov =
        jce_prefab_override::compute_overrides(s.scene, inst_e, src, src_e);
    jce_scene_destroy(src);

    int n = 0;
    for (const std::string &name : ov) {
        if (n >= max_names) break;
        snprintf(names[n], 64, "%s", name.c_str());
        n++;
    }
    return n;
}

int jce_state_get_prefab_field_overrides(uint32_t entity_id,
                                         const char *comp_name,
                                         char fields[][64], int max_fields)
{
    if (!comp_name || !comp_name[0]) return -1;
    if (!jce_state_entity_exists(entity_id)) return -1;
    if (!jce_state_entity_is_prefab(entity_id)) return -1;
    const char *path = jce_state_entity_prefab_path(entity_id);
    if (!path || path[0] == '\0') return -1;

    JceEntity inst_e = jce_state_to_ecs_entity(entity_id);
    if (inst_e == 0) return -1;

    JceEntity src_e = 0;
    JceScene *src = load_source_root(path, &src_e);
    if (!src) return -1;

    /* The unified "Light" override has no field localisation (its concrete
     * rows serialize with a NULL callback) — overridden_field_names returns
     * {} so the tooltip falls back to "whole component". */
    std::vector<std::string> fv =
        jce_prefab_override::overridden_field_names(
            s.scene, inst_e, src, src_e, comp_name);
    jce_scene_destroy(src);

    int n = 0;
    for (const std::string &f : fv) {
        if (n >= max_fields) break;
        snprintf(fields[n], 64, "%s", f.c_str());
        n++;
    }
    return n;
}

/* Query: is this entity's named component overridden vs its source? */
bool jce_state_is_prefab_component_overridden(uint32_t entity_id,
                                              const char *comp_name)
{
    if (!comp_name || !comp_name[0]) return false;
    if (!jce_state_entity_exists(entity_id)) return false;
    if (!jce_state_entity_is_prefab(entity_id)) return false;
    const char *path = jce_state_entity_prefab_path(entity_id);
    if (!path || path[0] == '\0') return false;

    JceEntity inst_e = jce_state_to_ecs_entity(entity_id);
    if (inst_e == 0) return false;

    JceEntity src_e = 0;
    JceScene *src = load_source_root(path, &src_e);
    if (!src) return false;
    bool ov = jce_prefab_override::is_component_overridden(
        s.scene, inst_e, src, src_e, comp_name);
    jce_scene_destroy(src);
    return ov;
}

/* ── Apply instance overrides to the source prefab (Phase 0.3) ────────
 *
 * Pushes this instance's current subtree back to its source .prefab.json,
 * so the prefab definition — and every FUTURE instantiation — reflects the
 * edits made on this instance.  Reuses the proven build_prefab_json_root +
 * write path that Save-As uses, but targets the instance's OWN linked path.
 * Variants keep their $variantOf parent link.
 *
 * LIMITATION (documented): this model serialises each instance's full state
 * (not a stored diff vs the source), so existing SIBLING instances are not
 * auto-updated by Apply — right-click a sibling → Revert to pull the new
 * source.  A stored-override model (siblings keep their own overrides and
 * inherit the rest) is the planned follow-up. */
bool jce_state_apply_prefab(uint32_t entity_id)
{
    if (!jce_state_entity_exists(entity_id))
        return false;
    if (!jce_state_entity_is_prefab(entity_id))
        return false;

    const char *path = jce_state_entity_prefab_path(entity_id);
    if (!path || path[0] == '\0')
        return false;
    char prefab_path[JCE_MAX_PREFAB_PATH];
    snprintf(prefab_path, sizeof(prefab_path), "%s", path);

    JceJson *root = build_prefab_json_root(entity_id);
    if (!root)
        return false;

    /* Preserve the variant link if this instance is itself a variant. */
    const char *vp = jce_state_get_variant_parent(entity_id);
    if (vp && vp[0] != '\0')
        jce_json_set_string(root, "$variantOf", vp);

    /* ed_write_json_to_file takes ownership of root (do not free after). */
    if (!ed_write_json_to_file(prefab_path, root)) {
        LOG_WARN(LOG_TAG, "prefab apply failed: %s", prefab_path);
        return false;
    }
    LOG_INFO(LOG_TAG, "prefab applied to source: %s", prefab_path);
    return true;
}
