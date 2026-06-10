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
#include "core/jce_editor_config.h"
#include "core/jce_build_manager.h"
#include "core/jce_editor_toast.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_scene_rendering_defaults.h"
#include "ui/jce_editor_panels.h"

extern "C" {
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_thread.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_pbr_material.h>
}

#include "scene/jce_asset_path_index.h"



#include <cmath>
#include <cstring>
#include <string>
#include <vector>

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
    /* Per-component disable bitmask (Unity-style enable toggles). Only written
     * when something is disabled. Bit count is well under 2^53 so double is
     * exact. */
    {
        uint64_t disabled = jce_scene_get_disabled_components(s.scene, e);
        if (disabled)
            jce_json_set_number(node, "disabledComponents", (double)disabled);
    }
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

/* ── Async post-save mesh-asset validation ────────────────────────────
 *
 * Verifying that every referenced glTF still loads through the engine's
 * cgltf path means an open-read + full CPU parse per mesh — pure I/O +
 * compute with no ECS / GPU touch.  That's exactly the work the engine's
 * own async loader keeps OFF the main thread (jce_scene_async.c).  We
 * snapshot the resolved mesh paths on the main thread (cheap ECS walk),
 * then read+parse them on a worker; results go to the engine logger,
 * which is thread-safe.  The round-trip validation stays synchronous
 * because it instantiates a flecs scene (entity creation is main-thread
 * only — same constraint the engine respects). */
struct MeshGatherCtx { std::vector<std::string> abs; std::vector<std::string> rel;
                       const char *scene_dir; };

static void mesh_gather_cb(JceScene *sc, JceEntity e, void *ud)
{
    auto *ctx = static_cast<MeshGatherCtx *>(ud);

    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(sc, e);
    if (!mr || mr->mesh_path[0] == '\0') return;
    if (!is_gltf_extension(mr->mesh_path)) return;

    char abs_path[1024];
    if (!jce_path_is_absolute(mr->mesh_path) && ctx->scene_dir && ctx->scene_dir[0]) {
        jce_path_join(abs_path, sizeof(abs_path), ctx->scene_dir, mr->mesh_path);
    } else {
        snprintf(abs_path, sizeof(abs_path), "%s", mr->mesh_path);
    }
    ctx->abs.emplace_back(abs_path);
    ctx->rel.emplace_back(mr->mesh_path);
}

struct MeshValidJob {
    std::vector<std::string> abs;   /* resolved absolute paths */
    std::vector<std::string> rel;   /* scene-relative, for logging */
    int                      checked = 0;
    int                      failed  = 0;
    JceAtomicI32            *done    = nullptr;   /* 0 running, 1 finished */
};

static JceThread    *g_mv_worker = nullptr;
static MeshValidJob *g_mv_job    = nullptr;

/* WORKER thread: read + cgltf-parse each mesh (no ECS / GPU). */
static void mesh_valid_worker(void *arg)
{
    MeshValidJob *j = (MeshValidJob *)arg;
    for (size_t i = 0; i < j->abs.size(); ++i) {
        uint64_t file_size = 0;
        void *data = jce_fs_host_read_all(j->abs[i].c_str(), &file_size);
        if (!data) continue;
        j->checked++;
        /* Pass the resolved absolute path so the loader resolves any
         * external .bin buffer from the asset's own directory. */
        JceModel *model = jce_model_load_gltf_memory(
            data, (uint32_t)file_size, j->abs[i].c_str());
        jce_free(data);
        if (!model) {
            j->failed++;
            LOG_WARN(LOG_TAG, "engine cgltf cannot load mesh '%s' — "
                     "runtime may fail to display this model",
                     j->rel[i].c_str());
        } else {
            jce_model_destroy(model);
        }
    }
    jce_atomic_i32_store(j->done, 1);
}

static void mesh_valid_finalize(void)
{
    MeshValidJob *j = g_mv_job;
    if (!j) return;
    if (g_mv_worker) { jce_thread_join(g_mv_worker); g_mv_worker = nullptr; }

    if (j->checked > 0 && j->failed == 0) {
        LOG_SUCCESS(LOG_TAG, "mesh asset validation passed: %d glTF "
                    "files verified with engine cgltf", j->checked);
    } else if (j->failed > 0) {
        LOG_WARN(LOG_TAG, "mesh asset validation: %d/%d glTF files "
                 "failed engine cgltf load", j->failed, j->checked);
    }

    if (j->done) jce_atomic_i32_destroy(j->done);
    delete j;
    g_mv_job = nullptr;
}

static void validate_mesh_assets(const char *scene_path)
{
    /* A prior validation still running: let it finish on its own (it
     * covers a near-identical scene state); skip starting a second. */
    if (g_mv_worker) return;

    char scene_dir[1024] = "";
    if (scene_path) {
        jce_path_parent(scene_dir, sizeof(scene_dir), scene_path);
    }

    MeshGatherCtx gctx;
    gctx.scene_dir = scene_dir[0] ? scene_dir : NULL;
    jce_scene_each_entity(s.scene, mesh_gather_cb, &gctx);
    if (gctx.abs.empty()) return;

    MeshValidJob *j = new MeshValidJob();
    j->abs  = std::move(gctx.abs);
    j->rel  = std::move(gctx.rel);
    j->done = jce_atomic_i32_create(0);
    g_mv_job = j;

    g_mv_worker = jce_thread_create(mesh_valid_worker, j, "jce_mesh_valid");
    if (!g_mv_worker) {
        /* No worker thread: run inline then finalise immediately. */
        mesh_valid_worker(j);
        mesh_valid_finalize();
    }
}

/* MAIN thread, per-frame: pick up a finished mesh validation. */
extern "C" void jce_state_scene_serial_poll(void)
{
    if (g_mv_job && jce_atomic_i32_load(g_mv_job->done) != 0)
        mesh_valid_finalize();
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
     * renderer that the material does NOT specify are preserved below.
     * Use VFS-aware check so bundle-relative material paths also work. */
    {
        JceFileSystem *_afs = jce_fs_get_active();
        bool mat_accessible = (_afs && jce_fs_exists(_afs, mr->material_path))
                              || jce_fs_host_exists_file(mr->material_path);
        if (!mat_accessible) return;
    }
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

    /* Self-contained-bundle warning: after relativization, an absolute path
     * (drive letter or UNC) means the asset lives outside the project root
     * and will NOT travel with a bundle.  Keep the path so the editor / Play
     * mode still resolves it on this machine; warn the user so they can
     * relocate the asset before packing a bundle. */
    bool is_abs = false;
    if (field[0] && field[1] == ':' &&
        ((field[0] >= 'A' && field[0] <= 'Z') || (field[0] >= 'a' && field[0] <= 'z'))) {
        is_abs = true;
    } else if (field[0] == '\\' && field[1] == '\\') {
        is_abs = true;
    } else if (field[0] == '/' && field[1] == '/') {
        is_abs = true;
    }
    if (is_abs) {
        LOG_WARN(LOG_TAG,
                 "scene references an asset outside the project root: '%s' "
                 "(kept; move it under the project root before packing a bundle)",
                 field);
    }
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
    if (JceCompoundColliderComponent *cc = jce_scene_get_compound_collider(scene, e)) {
        rel_in_place(cc->model_path, sizeof(cc->model_path), base);
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
    /* Relativize against the PROJECT ROOT, not the scene's parent dir.
     * Scenes commonly live in subdirectories (.jce/, scenes/) while
     * meshes/textures live under the project root sibling folders;
     * relative-to-scene-dir would produce "../foo/..." which the path
     * util rejects, leaking absolute paths into saved files. */
    const char *project = jce_editor_assets_get_project();
    char base[512] = {0};
    if (project && project[0]) {
        snprintf(base, sizeof(base), "%s", project);
    } else if (!jce_path_parent(base, sizeof(base), scene_path)) {
        return;
    }
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

    /* Persist as the most-recently used scene so the next editor launch
     * can re-open it automatically. */
    JceEditorConfig _ecfg;
    bool _ecfg_loaded = jce_editor_config_load(&_ecfg);
    if (_ecfg_loaded) {
        snprintf(_ecfg.last_scene_path, sizeof(_ecfg.last_scene_path),
                 "%s", scene_path);
        jce_editor_config_add_recent_scene(&_ecfg, scene_path);
        jce_editor_config_save(&_ecfg);
    }
    LOG_INFO(LOG_TAG, "scene saved to %s (%d entities)",
             scene_path, (int)g_entity_order.size());

    /* Auto-rebuild the game PAK so external ck.exe picks up the change
     * without a manual `cmake --build … --target PackGameAssets`.
     * Gated behind a Preferences toggle (default off) since the compile
     * can be slow on low-end machines.  CMake's mtime tracking makes the
     * cost near-zero when no inputs actually changed. */
    if (_ecfg_loaded && _ecfg.auto_repack_on_save &&
        _ecfg.build_preset[0] != '\0') {
        if (jce_build_manager_repack_game_assets(_ecfg.build_preset)) {
            jce_toast_info("%s", jce_editor_i18n("save.autoRepack.started"));
        } else {
            jce_toast_warn("%s", jce_editor_i18n("save.autoRepack.skipped"));
        }
    }
    return true;
}

/* ── Scene path accessor ─────────────────────────────────────────── */

const char *jce_state_get_current_scene_path(void)
{
    return s.current_scene_path;
}

/* ── Scene file load ─────────────────────────────────────────────── */

namespace { void close_active_bundle_mount(); }   /* fwd */

bool jce_state_load_scene_file(const char *scene_path)
{
    if (!scene_path || scene_path[0] == '\0')
        return false;

    /* Opening a scene during Play would swap the JceScene out from under a
     * running runtime — tear the runtime down first. */
    stop_play_before_scene_swap();

    /* Plain-file load: drop any bundle VFS override from a prior preview. */
    close_active_bundle_mount();

    bool ok = false;
    {
        HistorySuspendScope suspend;

        clear_scene_entities();

        ok = jce_scene_serial_load_file(s.scene, scene_path);
        if (ok) {
            jce_editor_scene_ensure_rendering_settings(s.scene);
            rebuild_entity_order_from_ecs();
            update_scene_dir_from_path(scene_path);
            set_current_scene_path_internal(scene_path);
            repair_scene_asset_paths();
            /* Persist as last-opened scene for next editor launch. */
            {
                JceEditorConfig _ecfg;
                if (jce_editor_config_load(&_ecfg)) {
                    snprintf(_ecfg.last_scene_path,
                             sizeof(_ecfg.last_scene_path),
                             "%s", scene_path);
                    jce_editor_config_add_recent_scene(&_ecfg, scene_path);
                    jce_editor_config_save(&_ecfg);
                }
            }
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

extern "C" {
#include <jce/resource/jce_bundle_loader.h>
}

/* ── Scene from bundle (read-only) ───────────────────────────────────
 *
 * We keep at most one bundle mounted at a time for scene-preview.
 * Opening another bundle (or a plain .scene.json) closes the previous
 * handle so the VFS doesn't accumulate stale mounts.
 */

namespace {
struct BundleMount {
    JceFileSystem    *fs   = nullptr;   /* private fs for this mount   */
    JceBundleFile    *bf   = nullptr;   /* single-file mode handle     */
    JceBundleCatalog *cat  = nullptr;   /* catalog mode handle         */
    std::string       active_bundle_id; /* mounted via cat (for unmount)*/
};
BundleMount g_bm;

bool ends_with_ci_local(const char *path, const char *suffix)
{
    if (!path || !suffix) return false;
    size_t n = std::strlen(path);
    size_t m = std::strlen(suffix);
    if (n < m) return false;
    const char *tail = path + n - m;
    for (size_t i = 0; i < m; ++i) {
        char a = tail[i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool bundle_path_from_sidecar(const char *sidecar, char *out, size_t outsz)
{
    if (!ends_with_ci_local(sidecar, ".jbundle.json") ||
        !out || outsz == 0) {
        return false;
    }
    size_t n = std::strlen(sidecar);
    size_t keep = n - std::strlen(".json");
    if (keep >= outsz)
        return false;
    std::memcpy(out, sidecar, keep);
    out[keep] = '\0';
    return true;
}

void close_active_bundle_mount()
{
    /* Clear global asset-loader fallback first so any in-flight read
     * doesn't hit a torn-down fs. */
    if (jce_fs_get_active() == g_bm.fs) jce_fs_set_active(nullptr);

    if (g_bm.cat) {
        if (!g_bm.active_bundle_id.empty())
            jce_bundle_unmount(g_bm.cat, g_bm.active_bundle_id.c_str());
        jce_bundle_catalog_close(g_bm.cat);
        g_bm.cat = nullptr;
    }
    if (g_bm.bf) {
        jce_bundle_file_close(g_bm.bf);
        g_bm.bf = nullptr;
    }
    if (g_bm.fs) {
        jce_fs_destroy(g_bm.fs);
        g_bm.fs = nullptr;
    }
    g_bm.active_bundle_id.clear();
}

bool apply_scene_bytes(const char *display_path,
                       const char *bytes, size_t size)
{
    /* Loading a scene (bundle / memory) during Play would swap the
     * JceScene out from under a running runtime — stop it first. */
    stop_play_before_scene_swap();

    bool ok = false;
    {
        HistorySuspendScope suspend;
        clear_scene_entities();
        ok = jce_scene_serial_load(s.scene, bytes, size);
        if (ok) {
            jce_editor_scene_ensure_rendering_settings(s.scene);
            rebuild_entity_order_from_ecs();
            update_scene_dir_from_path(display_path);
            set_current_scene_path_internal(display_path);
            repair_scene_asset_paths();
            LOG_INFO(LOG_TAG, "scene loaded from %s (%d entities)",
                     display_path, (int)g_entity_order.size());
        } else {
            LOG_WARN(LOG_TAG, "scene load failed: %s", display_path);
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
} /* namespace */

bool jce_state_load_scene_from_jbundle(const char *jbundle_path)
{
    if (!jbundle_path || jbundle_path[0] == '\0') return false;

    char sibling_bundle[1024];
    if (bundle_path_from_sidecar(jbundle_path, sibling_bundle,
                                 sizeof(sibling_bundle))) {
        if (!jce_fs_host_exists_file(sibling_bundle)) {
            LOG_WARN(LOG_TAG, "bundle sidecar has no sibling .jbundle: %s",
                     jbundle_path);
            return false;
        }
        LOG_INFO(LOG_TAG, "bundle sidecar selected; opening %s",
                 sibling_bundle);
        jbundle_path = sibling_bundle;
    }

    close_active_bundle_mount();
    g_bm.fs = jce_fs_create();
    if (!g_bm.fs) return false;

    g_bm.bf = jce_bundle_file_open(g_bm.fs, jbundle_path, nullptr);
    if (!g_bm.bf) {
        LOG_WARN(LOG_TAG, "open bundle failed: %s", jbundle_path);
        close_active_bundle_mount();
        return false;
    }
    const char *scene_vpath = jce_bundle_file_scene_path(g_bm.bf);
    if (!scene_vpath || scene_vpath[0] == '\0') {
        LOG_WARN(LOG_TAG, "bundle has no scene path: %s", jbundle_path);
        close_active_bundle_mount();
        return false;
    }
    uint64_t  sz  = 0;
    void     *raw = jce_fs_read_all(g_bm.fs, scene_vpath, &sz);
    if (!raw) {
        LOG_WARN(LOG_TAG, "cannot read scene '%s' from bundle %s",
                 scene_vpath, jbundle_path);
        close_active_bundle_mount();
        return false;
    }
    char display[1024];
    snprintf(display, sizeof(display), "bundle://%s!%s",
             jbundle_path, scene_vpath);
    jce_fs_set_active(g_bm.fs);
    bool ok = apply_scene_bytes(display, (const char *)raw, (size_t)sz);
    jce_fs_buffer_free(raw);
    if (!ok) close_active_bundle_mount();
    return ok;
}

bool jce_state_load_scene_from_catalog(const char *catalog_path,
                                       const char *bundle_id_or_scene)
{
    if (!catalog_path || catalog_path[0] == '\0') return false;

    char sibling_bundle[1024];
    if (bundle_path_from_sidecar(catalog_path, sibling_bundle,
                                 sizeof(sibling_bundle)))
        return jce_state_load_scene_from_jbundle(sibling_bundle);

    close_active_bundle_mount();
    g_bm.fs = jce_fs_create();
    if (!g_bm.fs) return false;

    g_bm.cat = jce_bundle_catalog_open(g_bm.fs, catalog_path);
    if (!g_bm.cat) {
        LOG_WARN(LOG_TAG, "open catalog failed: %s", catalog_path);
        close_active_bundle_mount();
        return false;
    }
    /* Resolve which bundle to mount. */
    const char *bundle_id = nullptr;
    if (bundle_id_or_scene && bundle_id_or_scene[0] != '\0') {
        /* Either a scene vpath (mount via scene_path lookup) or a
         * literal bundle id. */
        bundle_id = jce_bundle_mount_for_scene(g_bm.cat, bundle_id_or_scene);
        if (!bundle_id) {
            /* Treat as bundle id directly. */
            if (jce_bundle_mount(g_bm.cat, bundle_id_or_scene))
                bundle_id = bundle_id_or_scene;
        }
    } else {
        /* Pick the first scene-bundle. */
        uint32_t n = jce_bundle_catalog_count(g_bm.cat);
        for (uint32_t i = 0; i < n; ++i) {
            const char *id   = jce_bundle_catalog_id_at(g_bm.cat, i);
            const char *kind = id ? jce_bundle_catalog_kind(g_bm.cat, id) : nullptr;
            if (kind && strcmp(kind, "scene") == 0) {
                if (jce_bundle_mount(g_bm.cat, id)) { bundle_id = id; break; }
            }
        }
    }
    if (!bundle_id) {
        LOG_WARN(LOG_TAG, "catalog has no mountable scene bundle");
        close_active_bundle_mount();
        return false;
    }
    g_bm.active_bundle_id = bundle_id;

    const char *scene_vpath = jce_bundle_catalog_scene_path(g_bm.cat, bundle_id);
    if (!scene_vpath || scene_vpath[0] == '\0') {
        LOG_WARN(LOG_TAG, "bundle '%s' has no scene_path in catalog", bundle_id);
        close_active_bundle_mount();
        return false;
    }
    uint64_t  sz  = 0;
    void     *raw = jce_fs_read_all(g_bm.fs, scene_vpath, &sz);
    if (!raw) {
        LOG_WARN(LOG_TAG, "cannot read scene '%s' from catalog bundle %s",
                 scene_vpath, bundle_id);
        close_active_bundle_mount();
        return false;
    }
    char display[1024];
    snprintf(display, sizeof(display), "catalog://%s#%s",
             catalog_path, bundle_id);
    jce_fs_set_active(g_bm.fs);
    bool ok = apply_scene_bytes(display, (const char *)raw, (size_t)sz);
    jce_fs_buffer_free(raw);
    if (!ok) close_active_bundle_mount();
    return ok;
}
