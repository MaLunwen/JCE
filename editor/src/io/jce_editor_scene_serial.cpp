/*
 * jce_editor_scene_serial.cpp  Scene JSON serialization — thin wrapper.
 *
 * All component-level JSON serialization lives in the engine's
 * jce_scene_serial module.  This file handles editor-specific
 * concerns: prefab tree serialization, file I/O with dialogs,
 * undo/redo snapshot integration, mesh asset validation, and
 * round-trip verification.
 */

#include "jce_editor_file_util.h"
#include "core/jce_editor_state_internal.h"
#include "ui/jce_editor_panels.h"

extern "C" {
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_pbr_material.h>
}

#include "scene/jce_asset_path_index.h"



#include <cmath>
#include <cstring>

/* ── Serialize entity tree to JSON (recursive, for prefabs) ────────── */

JceJson *serialize_entity_tree_json(uint32_t entity_id)
{
    if (!s.scene || entity_id == 0) return NULL;
    JceEntity e = (JceEntity)entity_id;
    JceEditorMeta *meta = jce_scene_get_editor_meta(s.scene, e);
    if (!meta) return NULL;

    JceJson *node = jce_json_object();
    if (!node) return NULL;

    jce_json_set_string(node, "name", meta->name);
    jce_json_set_bool(node, "enabled", meta->enabled);
    jce_json_set_number(node, "tagColor", (double)meta->tag_color);
    if (meta->tag[0] != '\0')
        jce_json_set_string(node, "tag", meta->tag);
    if (meta->prefab_instance) {
        jce_json_set_bool(node, "prefabInstance", true);
        if (meta->prefab_path[0] != '\0')
            jce_json_set_string(node, "prefabPath", meta->prefab_path);
    }

    /* Delegate component serialization to the engine. */
    {
        JceJson *comps = jce_scene_serialize_entity_components(s.scene, e);
        if (comps)
            jce_json_set_child(node, "components", comps);
    }

    JceJson *children = jce_json_array();
    if (!children) {
        jce_json_free(node);
        return NULL;
    }
    jce_json_set_child(node, "children", children);

    JceEntity child_buf[JCE_MAX_CHILDREN];
    int cn = jce_scene_get_children(s.scene, e, child_buf, JCE_MAX_CHILDREN);
    for (int i = 0; i < cn; i++) {
        JceJson *child = serialize_entity_tree_json((uint32_t)child_buf[i]);
        if (child)
            jce_json_array_push(children, child);
    }

    return node;
}

/* ── Build full scene JSON root (delegates to engine) ──────────────── */

JceJson *build_scene_json_root(void)
{
    /* The engine serializer emits the contract envelope format. */
    return jce_scene_save_json(s.scene);
}

JceJson *build_prefab_json_root(uint32_t entity_id)
{
    JceJson *root = jce_json_object();
    JceJson *contract = jce_json_object();
    JceJson *prefab = jce_json_object();
    JceJson *root_node = serialize_entity_tree_json(entity_id);
    if (!root || !contract || !prefab || !root_node) {
        jce_json_free(root);
        jce_json_free(contract);
        jce_json_free(prefab);
        jce_json_free(root_node);
        return NULL;
    }

    jce_json_set_child(root, JCE_SCENE_CONTRACT_KEY, contract);
    jce_json_set_string(contract, JCE_SCENE_CONTRACT_NAME_KEY,
                        JCE_SCENE_CONTRACT_NAME);
    jce_json_set_int(contract, JCE_SCENE_CONTRACT_MAJOR_KEY,
                     JCE_SCENE_CONTRACT_MAJOR);
    jce_json_set_int(contract, JCE_SCENE_CONTRACT_MINOR_KEY,
                     JCE_SCENE_CONTRACT_MINOR);

    jce_json_set_child(root, "prefab", prefab);
    jce_json_set_int(prefab, JCE_SCENE_VERSION_KEY,
                     JCE_SCENE_CONTRACT_MAJOR);
    jce_json_set_child(prefab, "root", root_node);
    return root;
}

/* ── Find prefab root node in parsed JSON ────────────────────────── */

const JceJson *find_prefab_root_node(const JceJson *root)
{
    if (!root) return NULL;

    if (jce_json_is_object(root)) {
        const JceJson *prefab = jce_json_get(root, "prefab");
        if (jce_json_is_object(prefab)) {
            const JceJson *node = jce_json_get(prefab, "root");
            if (jce_json_is_object(node))
                return node;
        }
        if (looks_like_entity_object(root))
            return root;
    }

    return NULL;
}

/* ── Mark entity subtree as prefab instance ──────────────────────── */

void mark_prefab_instance_recursive(uint32_t entity_id, const char *prefab_path)
{
    if (!s.scene || entity_id == 0) return;
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)entity_id);
    if (!m) return;

    if (prefab_path && prefab_path[0] != '\0') {
        m->prefab_instance = true;
        snprintf(m->prefab_path, sizeof(m->prefab_path), "%s", prefab_path);
    } else {
        m->prefab_instance = false;
        m->prefab_path[0] = '\0';
    }

    uint32_t child_ids[JCE_MAX_CHILDREN];
    int child_count = jce_state_entity_children(entity_id, child_ids, JCE_MAX_CHILDREN);
    for (int i = 0; i < child_count; i++)
        mark_prefab_instance_recursive(child_ids[i], prefab_path);
}

/* ── Mesh asset validation (glTF/GLB via engine cgltf) ────────────── */

static bool is_gltf_extension(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    return (strcmp(dot, ".gltf") == 0 || strcmp(dot, ".glb") == 0 ||
            strcmp(dot, ".GLTF") == 0 || strcmp(dot, ".GLB") == 0);
}

struct MeshValidCtx { int checked; int failed; const char *scene_dir; };

static void validate_mesh_cb(JceScene *sc, JceEntity e, void *ud)
{
    auto *ctx = static_cast<MeshValidCtx *>(ud);

    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(sc, e);
    if (!mr || mr->mesh_path[0] == '\0') return;
    if (!is_gltf_extension(mr->mesh_path)) return;

    char abs_path[1024];
    if (!jce_path_is_absolute(mr->mesh_path) && ctx->scene_dir && ctx->scene_dir[0]) {
        jce_path_join(abs_path, sizeof(abs_path), ctx->scene_dir, mr->mesh_path);
    } else {
        snprintf(abs_path, sizeof(abs_path), "%s", mr->mesh_path);
    }

    uint64_t file_size = 0;
    void *data = jce_fs_host_read_all(abs_path, &file_size);
    if (!data) return;

    ctx->checked++;
    JceModel *model = jce_model_load_gltf_memory(data, (uint32_t)file_size,
                                                  mr->mesh_path);
    jce_free(data);

    if (!model) {
        ctx->failed++;
        LOG_WARN(LOG_TAG, "engine cgltf cannot load mesh '%s' — "
                 "runtime may fail to display this model",
                 mr->mesh_path);
    } else {
        jce_model_destroy(model);
    }
}

static void validate_mesh_assets(const char *scene_path)
{
    char scene_dir[1024] = "";
    if (scene_path) {
        jce_path_parent(scene_dir, sizeof(scene_dir), scene_path);
    }

    MeshValidCtx ctx = { 0, 0, scene_dir[0] ? scene_dir : NULL };
    jce_scene_each_entity(s.scene, validate_mesh_cb, &ctx);

    if (ctx.checked > 0 && ctx.failed == 0) {
        LOG_SUCCESS(LOG_TAG, "mesh asset validation passed: %d glTF "
                    "files verified with engine cgltf", ctx.checked);
    } else if (ctx.failed > 0) {
        LOG_WARN(LOG_TAG, "mesh asset validation: %d/%d glTF files "
                 "failed engine cgltf load", ctx.failed, ctx.checked);
    }
}

/* ── Post-load asset path repair (O(1) per path via asset index) ──── */
struct PathRepairCtx { int mesh_repaired; int mat_repaired; int mat_backfilled; };

static void repair_paths_cb(JceScene * /*sc*/, JceEntity e, void *ud)
{
    auto *ctx = static_cast<PathRepairCtx *>(ud);
    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(s.scene, e);
    if (!mr) return;

    char resolved[512];

    if (mr->mesh_path[0] != '\0' && !jce_fs_host_exists_file(mr->mesh_path)) {
        if (jce_asset_path_index_lookup(mr->mesh_path, resolved, (int)sizeof(resolved))) {
            snprintf(mr->mesh_path, sizeof(mr->mesh_path), "%s", resolved);
            ctx->mesh_repaired++;
        }
    }

    if (mr->material_path[0] == '\0') return;

    bool repaired_mat = false;
    if (!jce_fs_host_exists_file(mr->material_path)) {
        if (jce_asset_path_index_lookup(mr->material_path, resolved, (int)sizeof(resolved))) {
            snprintf(mr->material_path, sizeof(mr->material_path), "%s", resolved);
            ctx->mat_repaired++;
            repaired_mat = true;
        }
    }

    /* Re-run material backfill so baseColor / texture refs get applied
     * even when the engine-side parser couldn't resolve the path.  We
     * always reapply when a real material file exists — material is the
     * single source of truth (Unity semantics).  Texture refs on the
     * renderer that the material does NOT specify are preserved below. */
    if (!jce_fs_host_exists_file(mr->material_path)) return;
    (void)repaired_mat;

    JcePbrMaterial pbr = {};
    char tex_paths[5][256] = {};
    if (!jce_pbr_material_load_json(mr->material_path, &pbr, tex_paths))
        return;

    if (tex_paths[0][0]) snprintf(mr->albedo_tex, sizeof(mr->albedo_tex), "%s", tex_paths[0]);
    if (tex_paths[1][0]) snprintf(mr->mr_tex, sizeof(mr->mr_tex), "%s", tex_paths[1]);
    if (tex_paths[2][0]) snprintf(mr->normal_tex, sizeof(mr->normal_tex), "%s", tex_paths[2]);
    if (tex_paths[3][0]) snprintf(mr->ao_tex, sizeof(mr->ao_tex), "%s", tex_paths[3]);
    if (tex_paths[4][0]) snprintf(mr->emissive_tex, sizeof(mr->emissive_tex), "%s", tex_paths[4]);
    mr->base_color[0] = pbr.base_color_factor[0];
    mr->base_color[1] = pbr.base_color_factor[1];
    mr->base_color[2] = pbr.base_color_factor[2];
    mr->base_color[3] = pbr.base_color_factor[3];
    mr->metallic     = pbr.metallic_factor;
    mr->roughness    = pbr.roughness_factor;
    mr->emissive[0]  = pbr.emissive_factor[0];
    mr->emissive[1]  = pbr.emissive_factor[1];
    mr->emissive[2]  = pbr.emissive_factor[2];
    mr->normal_scale = pbr.normal_scale;
    mr->ao_strength  = pbr.ao_strength;
    ctx->mat_backfilled++;
}

static void repair_scene_asset_paths(void)
{
    if (!s.scene) return;
    if (jce_asset_path_index_size() == 0) return;
    PathRepairCtx ctx = { 0, 0, 0 };
    jce_scene_each_entity(s.scene, repair_paths_cb, &ctx);
    if (ctx.mesh_repaired || ctx.mat_repaired || ctx.mat_backfilled) {
        LOG_INFO(LOG_TAG,
                 "scene asset paths repaired: mesh=%d mat=%d mat_backfill=%d",
                 ctx.mesh_repaired, ctx.mat_repaired, ctx.mat_backfilled);
    }
}

/* ── Round-trip validation ────────────────────────────────────────── */

static void count_entity_cb(JceScene * /*s*/, JceEntity /*e*/, void *ud)
{
    (*(int *)ud)++;
}

static bool validate_scene_round_trip(const char *path)
{
    JceScene *verify = jce_scene_create();
    if (!verify) {
        LOG_ERROR(LOG_TAG, "round-trip validation: cannot create temp scene");
        return false;
    }

    bool ok = jce_scene_serial_load_file(verify, path);
    if (!ok) {
        LOG_ERROR(LOG_TAG, "round-trip validation FAILED: saved file cannot "
                  "be loaded back (%s)", path);
        jce_scene_destroy(verify);
        return false;
    }

    int loaded_count = 0;
    jce_scene_each_entity(verify, count_entity_cb, &loaded_count);

    int expected = (int)g_entity_order.size();
    if (loaded_count != expected) {
        LOG_WARN(LOG_TAG, "round-trip validation: entity count mismatch "
                 "(saved %d, loaded %d) in %s",
                 expected, loaded_count, path);
    } else {
        LOG_SUCCESS(LOG_TAG, "round-trip validation passed: %d entities (%s)",
                    loaded_count, path);
    }

    jce_scene_destroy(verify);
    return true;
}

/* ── Path relativization sweep (#4) ──────────────────────────────── */

static void rel_in_place(char *field, size_t cap, const char *base_dir)
{
    if (!field || field[0] == '\0') return;
    char tmp[1024];
    jce_editor_path_to_relative_to(tmp, sizeof(tmp), field, base_dir);
    if (tmp[0]) snprintf(field, cap, "%s", tmp);
}

struct RelSweepCtx { JceScene *scene; const char *base_dir; };

static void normalize_entity_paths_cb(JceScene *sc, JceEntity e, void *ud)
{
    RelSweepCtx *ctx = (RelSweepCtx *)ud;
    (void)sc;
    const char *base = ctx->base_dir;
    JceScene *scene = ctx->scene;

    JceEditorMeta *meta = jce_scene_get_editor_meta(scene, e);
    if (meta) rel_in_place(meta->prefab_path, sizeof(meta->prefab_path), base);

    if (JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e)) {
        rel_in_place(mr->mesh_path,     sizeof(mr->mesh_path),     base);
        rel_in_place(mr->material_path, sizeof(mr->material_path), base);
        rel_in_place(mr->albedo_tex,    sizeof(mr->albedo_tex),    base);
        rel_in_place(mr->mr_tex,        sizeof(mr->mr_tex),        base);
        rel_in_place(mr->normal_tex,    sizeof(mr->normal_tex),    base);
        rel_in_place(mr->ao_tex,        sizeof(mr->ao_tex),        base);
        rel_in_place(mr->emissive_tex,  sizeof(mr->emissive_tex),  base);
    }
    if (JceSpriteRendererComponent *sr = jce_scene_get_sprite_renderer(scene, e)) {
        rel_in_place(sr->sprite_path, sizeof(sr->sprite_path), base);
    }
    if (JceSpriteAnimatorComponent *sa = jce_scene_get_sprite_animator(scene, e)) {
        rel_in_place(sa->sheet_path, sizeof(sa->sheet_path), base);
        rel_in_place(sa->atlas_path, sizeof(sa->atlas_path), base);
    }
    if (JceSkeletalAnimatorComponent *ska = jce_scene_get_skeletal_animator(scene, e)) {
        rel_in_place(ska->skeleton_path, sizeof(ska->skeleton_path), base);
    }
    if (JceScriptComponent *scp = jce_scene_get_script(scene, e)) {
        rel_in_place(scp->script_path, sizeof(scp->script_path), base);
    }
    if (JceSkyboxComponent *sky = jce_scene_get_skybox(scene, e)) {
        rel_in_place(sky->hdr_path, sizeof(sky->hdr_path), base);
    }
}

static void normalize_all_scene_paths_to_relative(const char *scene_path)
{
    if (!s.scene || !scene_path) return;
    char base[512] = {0};
    if (!jce_path_parent(base, sizeof(base), scene_path)) return;
    RelSweepCtx ctx{ s.scene, base };
    jce_scene_each_entity(s.scene, normalize_entity_paths_cb, &ctx);
}

/* ── Scene file save ─────────────────────────────────────────────── */

bool jce_state_save_scene_file(const char *scene_path)
{
    if (!scene_path || scene_path[0] == '\0')
        return false;

    /* Convert all path fields to scene-relative before writing. */
    normalize_all_scene_paths_to_relative(scene_path);

    if (!jce_scene_serial_save_file(s.scene, scene_path)) {
        LOG_WARN(LOG_TAG, "scene save failed: %s", scene_path);
        return false;
    }

    validate_scene_round_trip(scene_path);
    validate_mesh_assets(scene_path);

    update_scene_dir_from_path(scene_path);
    set_current_scene_path_internal(scene_path);
    s.scene_modified = false;
    LOG_INFO(LOG_TAG, "scene saved to %s (%d entities)",
             scene_path, (int)g_entity_order.size());
    return true;
}

/* ── Scene path accessor ─────────────────────────────────────────── */

const char *jce_state_get_current_scene_path(void)
{
    return s.current_scene_path;
}

/* ── Scene file load ─────────────────────────────────────────────── */

bool jce_state_load_scene_file(const char *scene_path)
{
    if (!scene_path || scene_path[0] == '\0')
        return false;

    bool ok = false;
    {
        HistorySuspendScope suspend;

        clear_scene_entities();

        ok = jce_scene_serial_load_file(s.scene, scene_path);
        if (ok) {
            rebuild_entity_order_from_ecs();
            update_scene_dir_from_path(scene_path);
            set_current_scene_path_internal(scene_path);
            repair_scene_asset_paths();
            LOG_INFO(LOG_TAG, "scene loaded from %s (%d entities)",
                     scene_path, (int)g_entity_order.size());
        } else {
            LOG_WARN(LOG_TAG, "scene load failed: %s", scene_path);
        }
    }
    if (ok) {
        s_undo_history.clear();
        s_redo_history.clear();
        s.scene_modified = false;
        s_history_edit_nesting = 0;
        s_history_outer_edit_pushed_snapshot = false;
        s_history_manual_batch_depth = 0;
        s_transaction.active = false;
        s_transaction.label[0] = '\0';
        s_transaction.before.scene_json.clear();
        s_transaction.before.scene_path.clear();
    }
    return ok;
}
