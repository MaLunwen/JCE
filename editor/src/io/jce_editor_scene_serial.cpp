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
#include "core/jce_editor_project_state.h"   /* per-project last_scene */
#include "core/jce_build_manager.h"
#include "core/jce_editor_toast.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_scene_rendering_defaults.h"
#include "scene/jce_editor_scene_render.h"
#include "scene/jce_editor_scene_asset_cache.h"
#include "ui/jce_editor_panels.h"

extern "C" {
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/middleware/scene/jce_scene.h>   /* jce_scene_particles_set_asset_root */
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_pbr_material.h>
}

#include "scene/jce_asset_path_index.h"
#include "io/jce_editor_prefab_override.h"



#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <jce/os/core/jce_str.h>
#include "core/jce_editor_project.h"

/* Project-root switch (dialogs/jce_dialog_project.cpp) — reloads the
 * manifest, asset DB root, PAK key, and game string tables. */
void set_current_project_root(const char *path);

/* Remember the just-loaded/saved scene as this project's resume target
 * (per-project store), alongside the global last_scene_path that drives
 * cold-boot restore.  No-op when no project store is active. */
static void mirror_last_scene_to_project(const char *scene_path)
{
    if (scene_path && scene_path[0] && jce_editor_pstate_active())
        jce_editor_pstate_set_str("last_scene", scene_path);
}

/* ── Project-root follow on scene open ─────────────────────────────
 *
 * Opening a scene IS opening its project.  Without this, the project
 * root stays at whatever was opened last (often a different project
 * from recents) and every root-based mechanism silently degrades:
 * path relativization falls back to absolute paths, the model
 * importer's extracted textures stay CWD-relative, and the bundle
 * packer treats the project's own assets as "external".  Walk up from
 * the scene file to the owning jce_project.json and follow it. */
static void follow_scene_project_root(const char *scene_path)
{
    char dir[512];
    if (!jce_path_parent(dir, sizeof(dir), scene_path) || !dir[0])
        return;

    for (int up = 0; up < 8 && dir[0]; ++up) {
        char manifest[600];
        snprintf(manifest, sizeof(manifest), "%s/jce_project.json", dir);
        if (jce_fs_host_exists_file(manifest)) {
            char norm[512];
            if (!jce_path_normalize(norm, sizeof(norm), dir))
                snprintf(norm, sizeof(norm), "%s", dir);
            for (char *q = norm; *q; ++q) if (*q == '\\') *q = '/';

            /* Switch manifest-driven subsystems only on a real change. */
            extern char s_current_project_root[512];
            if (jce_strcasecmp(s_current_project_root, norm) != 0) {
                LOG_INFO(LOG_TAG,
                         "scene belongs to project '%s' — following as "
                         "current project root", norm);
                set_current_project_root(norm);
            }

            /* The canonical asset base (what saved refs are relative to)
             * is <project>/<source_assets>, not the project dir itself. */
            const JceProject *jp = jce_editor_project_get();
            const char *src = (jp && jp->source_assets && jp->source_assets[0])
                              ? jp->source_assets : "assets";
            char assets_base[700];
            snprintf(assets_base, sizeof(assets_base), "%s/%s", norm, src);
            if (!jce_fs_host_exists_dir(assets_base))
                snprintf(assets_base, sizeof(assets_base), "%s", norm);
            const char *cur = jce_editor_assets_get_project();
            if (!cur || jce_strcasecmp(cur, assets_base) != 0)
                jce_editor_assets_set_project(assets_base);

            /* Bind render settings and component-relative assets before the
             * scene starts building lazy renderer/particle state. */
            jce_editor_scene_render_refresh_content_context();
            return;
        }
        char parent[512];
        if (!jce_path_parent(parent, sizeof(parent), dir) ||
            strcmp(parent, dir) == 0)
            return;
        snprintf(dir, sizeof(dir), "%s", parent);
    }
}

/* ── Prefab source instantiation (scratch scene, for override diff) ────
 *
 * Load a prefab .prefab.json into a private throwaway scene and parse its
 * root node's components onto a single scratch entity.  Returns that
 * entity (the caller diffs the live instance against it, then destroys the
 * scratch scene).  Children are NOT instantiated — per-component override
 * tracking is root-level for the MVP.  Returns 0 on any failure (caller
 * then falls back to the legacy full snapshot — backward compatible). */
struct PrefabSource {
    JceScene *scene = nullptr;
    JceEntity root  = 0;
};

static bool load_prefab_source(const char *prefab_path, PrefabSource *out)
{
    if (!prefab_path || !prefab_path[0] || !out) return false;

    JceJson *root_json = jce_json_parse_file(prefab_path);
    if (!root_json) return false;

    const JceJson *node = find_prefab_root_node(root_json);
    if (!node) { jce_json_free(root_json); return false; }

    JceScene *scratch = jce_scene_create();
    if (!scratch) { jce_json_free(root_json); return false; }

    JceEntity e = jce_scene_create_entity(scratch, "prefab_source");
    if (e == 0) {
        jce_scene_destroy(scratch);
        jce_json_free(root_json);
        return false;
    }
    /* Overlay the source's components onto the scratch entity through the
     * engine's per-component parsers (same path the live instantiation
     * uses), so the byte layout matches the live instance exactly. */
    jce_scene_parse_entity_json(scratch, e, node);
    jce_json_free(root_json);

    out->scene = scratch;
    out->root  = e;
    return true;
}

static void free_prefab_source(PrefabSource *src)
{
    if (src && src->scene) {
        jce_scene_destroy(src->scene);
        src->scene = nullptr;
        src->root  = 0;
    }
}

/* ── Serialize entity tree to JSON (recursive, for prefabs) ──────────
 *
 * `emit_overrides`: when true AND the entity is a prefab instance whose
 * source .prefab.json loads, emit the additive Unity/Godot override form
 * (an "overrides":[componentNames] array + ONLY the differing components)
 * instead of the full component snapshot.  When false (prefab-FILE save,
 * clipboard copy) or the source cannot be loaded, the legacy full snapshot
 * is written — so existing callers and legacy files are byte-unchanged. */
static JceJson *serialize_entity_tree_json_ex(uint32_t entity_id,
                                              bool emit_overrides);

JceJson *serialize_entity_tree_json(uint32_t entity_id)
{
    /* Default callers (prefab-file save, copy/paste clipboard) keep the
     * full-snapshot behaviour — only an explicit override-aware caller
     * opts into the diff form. */
    return serialize_entity_tree_json_ex(entity_id, false);
}

/* Override-aware scene serializer: embeds prefab instances as override
 * diffs.  Used by the tree-format scene writer + the override test. */
JceJson *serialize_entity_tree_json_overrides(uint32_t entity_id)
{
    return serialize_entity_tree_json_ex(entity_id, true);
}

static JceJson *serialize_entity_tree_json_ex(uint32_t entity_id,
                                              bool emit_overrides)
{
    if (!s.scene || entity_id == 0) return NULL;
    JceEntity e = (JceEntity)entity_id;
    JceEditorMeta *meta = jce_scene_get_editor_meta(s.scene, e);
    if (!meta) return NULL;

    JceJson *node = jce_json_object();
    if (!node) return NULL;

    jce_json_set_string(node, "name", meta->name);
    jce_json_set_bool(node, "enabled", meta->enabled);
    /* Per-component enable toggles.  The scan lives in the engine now
     * (jce_scene_write_disabled_components): this file used to carry a
     * hand-copied duplicate whose comment ASSERTED it matched the
     * engine writer — a claim maintained by hand, which is exactly how
     * the two drift. */
    jce_scene_write_disabled_components(node, s.scene, e);

    jce_json_set_number(node, "tagColor", (double)meta->tag_color);
    if (meta->tag[0] != '\0')
        jce_json_set_string(node, "tag", meta->tag);
    /* Entity LAYER (the ECS component, NOT JceEditorMeta.layer).  Same
     * gate and same source as the engine writer — shared outright now
     * rather than mirrored, so "byte-identical nodes" is a property of
     * one implementation instead of a promise in two comments. */
    jce_scene_write_entity_layer(node, s.scene, e);

    if (meta->prefab_instance) {
        jce_json_set_bool(node, "prefabInstance", true);
        if (meta->prefab_path[0] != '\0')
            jce_json_set_string(node, "prefabPath", meta->prefab_path);
    }

    /* Component serialization.
     *
     * Override path: a prefab INSTANCE with a loadable source emits the
     * additive "overrides":[...] array + ONLY the components that differ
     * byte-wise from the source (Transform is diffed like any other row).
     * Everything else (regular entities, instances whose source is
     * missing, prefab-FILE saves, clipboard copies) falls through to the
     * legacy FULL snapshot — keeping old scenes/files byte-identical. */
    bool wrote_overrides = false;
    if (emit_overrides && meta->prefab_instance && meta->prefab_path[0]) {
        PrefabSource src;
        if (load_prefab_source(meta->prefab_path, &src)) {
            wrote_overrides = jce_prefab_override::write_override_node(
                node, s.scene, e, src.scene, src.root);
            free_prefab_source(&src);
        }
    }
    if (!wrote_overrides) {
        /* Delegate full component serialization to the engine. */
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

    /* In override mode an instance root's children come ENTIRELY from the
     * source on load (instantiate_prefab rebuilds the subtree), so we do
     * NOT re-serialize them — that would duplicate them on reload.  The
     * override MVP is root-level; per-child overrides are a follow-up.
     * Regular entities (and the full-snapshot path) recurse as before. */
    if (!wrote_overrides) {
        JceEntity child_buf[JCE_MAX_CHILDREN];
        int cn = jce_scene_get_children(s.scene, e, child_buf,
                                        JCE_MAX_CHILDREN);
        for (int i = 0; i < cn; i++) {
            JceJson *child = serialize_entity_tree_json_ex(
                (uint32_t)child_buf[i], emit_overrides);
            if (child)
                jce_json_array_push(children, child);
        }
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
    if (JceSequencePlayerComponent *sp = jce_scene_get_sequence_player(scene, e)) {
        rel_in_place(sp->seq_path, sizeof(sp->seq_path), base);
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
    return jce_state_save_scene_file_ex(scene_path, 0);
}

bool jce_state_save_scene_file_ex(const char *scene_path, uint32_t flags)
{
    if (!scene_path || scene_path[0] == '\0')
        return false;

    /* The Sequencer panel's live preview writes evaluated track values
     * straight into scene components; restore the authored originals
     * before serializing so previewed values never reach disk. */
    jce_panel_sequencer_preview_flush();

    /* SAVE SAFETY: the world-streaming preview spawns chunk entities
     * directly into the live scene.  Destroy the streamer (which removes
     * every streamed entity) BEFORE serializing so streamed chunk content
     * never bakes into the main scene file; rebuild right after. */
    jce_editor_scene_render_streaming_teardown();

    /* Convert all path fields to scene-relative before writing. */
    normalize_all_scene_paths_to_relative(scene_path);

    const bool save_ok = jce_scene_serial_save_file(s.scene, scene_path);
    jce_editor_scene_render_streaming_rebuild();
    if (!save_ok) {
        LOG_WARN(LOG_TAG, "scene save failed: %s", scene_path);
        return false;
    }

    /* Autosave (JCE_SAVE_AUTOSAVE): skip the synchronous full-scene
     * round-trip re-load — it is a purely diagnostic load-after-save (the
     * return value is discarded) that roughly doubles the save hitch on a
     * big scene, and the periodic timer save does not need it.  Explicit
     * manual saves keep the full validation. */
    if (!(flags & JCE_SAVE_AUTOSAVE))
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
    mirror_last_scene_to_project(scene_path);
    LOG_INFO(LOG_TAG, "scene saved to %s (%d entities)",
             scene_path, (int)g_entity_order.size());

    /* Auto-rebuild the game PAK so external ck.exe picks up the change
     * without a manual `cmake --build … --target PackGameAssets`.
     * Gated behind a Preferences toggle (default off) since the compile
     * can be slow on low-end machines.  CMake's mtime tracking makes the
     * cost near-zero when no inputs actually changed. */
    if (!(flags & JCE_SAVE_AUTOSAVE) &&
        _ecfg_loaded && _ecfg.auto_repack_on_save &&
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

    /* Restore any Sequencer live-preview values BEFORE the swap — after
     * it the cached entity ids would point into the new scene. */
    jce_panel_sequencer_preview_flush();

    /* Opening a scene during Play would swap the JceScene out from under a
     * running runtime — tear the runtime down first. */
    stop_play_before_scene_swap();

    /* The streaming-preview streamer holds entity handles into the old
     * scene — destroy it BEFORE the entities go away. */
    jce_editor_scene_render_streaming_teardown();

    /* Plain-file load: drop any bundle VFS override from a prior preview. */
    close_active_bundle_mount();

    bool ok = false;
    {
        HistorySuspendScope suspend;

        clear_scene_entities();

        /* Scene-switch cache reset: the renderer/pick model caches and the
         * negative resolve-miss cache survive a scene swap and cache load
         * FAILURES, so a model that failed (or whose basename collided with a
         * missing asset) under the previous scene would never reload without an
         * editor restart.  Drop them so the new scene resolves + loads fresh —
         * this is what makes switching scenes behave like a fresh start. */
        jce_editor_scene_render_invalidate_model_caches();
        jce_editor_scene_asset_cache_clear_resolve_misses();

        ok = jce_scene_serial_load_file(s.scene, scene_path);
        if (ok) {
            jce_editor_scene_ensure_rendering_settings(s.scene);
            rebuild_entity_order_from_ecs();
            update_scene_dir_from_path(scene_path);
            set_current_scene_path_internal(scene_path);
            follow_scene_project_root(scene_path);
            repair_scene_asset_paths();
            /* Persist as last-opened scene for next editor launch.  Skip the
             * write when nothing actually changed — on the startup
             * auto-restore the scene is already last_scene_path AND already
             * at the front of the recents, so re-serializing both config
             * files would be a pure redundant disk hit on the
             * time-to-first-frame path. */
            {
                JceEditorConfig _ecfg;
                if (jce_editor_config_load(&_ecfg)) {
                    const bool path_changed =
                        strcmp(_ecfg.last_scene_path, scene_path) != 0;
                    const bool was_front =
                        _ecfg.recent_scene_count > 0 &&
                        strcmp(_ecfg.recent_scene_paths[0], scene_path) == 0;
                    snprintf(_ecfg.last_scene_path,
                             sizeof(_ecfg.last_scene_path),
                             "%s", scene_path);
                    jce_editor_config_add_recent_scene(&_ecfg, scene_path);
                    if (path_changed || !was_front)
                        jce_editor_config_save(&_ecfg);
                }
            }
            mirror_last_scene_to_project(scene_path);
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

        /* Land the viewport camera where the user last left it in THIS
         * scene (no stored pose = keep the current framing). */
        jce_editor_scene_camera_restore_pose(scene_path);

        /* Show the streamed world in the editor scene view for streaming scenes
         * (auto-enables preview so the editor matches Play instead of looking
         * empty); user can toggle it off in the World Streaming panel. */
        jce_editor_scene_render_streaming_autostart();
    }
    return ok;
}

/* ── Frame-sliced (non-blocking) scene-file open ─────────────────────
 *
 * For very large scenes (street_demo: ~17.5k entities, 20 MB) the
 * synchronous open above creates every entity in one call and blocks the
 * editor's first frame.  This path parses the JSON up front (the smaller
 * half of the cost) then drives the engine's incremental loader
 * (jce_scene_load_stream_*) a chunk of entities per editor frame, keeping
 * the frame loop alive.  Editor interaction is gated by a modal overlay
 * (jce_editor_layout.cpp) until the load reaches DONE, so nothing ever
 * operates on a half-built scene.  The post-load finalize is the SAME set
 * of steps the synchronous path runs.
 *
 * Threshold: scenes with <= the cutoff load synchronously so the common
 * case (small scenes) stays instant with no overlay flicker. */
extern "C" {
#include <jce/resource/jce_scene_contract.h>
}

namespace {

constexpr int   kAsyncEntityThreshold = 2000;  /* below this: load sync   */
constexpr int   kEntitiesPerSlice     = 128;   /* batch between budget checks */

struct DeferredSceneLoad {
    bool                active     = false;
    JceJson            *root       = nullptr;  /* owned until finalize     */
    JceSceneLoadStream *stream     = nullptr;  /* engine stream handle     */
    int                 total      = 0;
    std::string         scene_path;
};
DeferredSceneLoad g_dsl;

/* Run the editor-side finalize steps that follow a successful engine load —
 * identical to the tail of jce_state_load_scene_file().  Called once the
 * incremental create pass + engine ref-fixups have completed. */
void finalize_deferred_scene_load()
{
    const std::string path = g_dsl.scene_path;

    {
        HistorySuspendScope suspend;

        jce_editor_scene_ensure_rendering_settings(s.scene);
        rebuild_entity_order_from_ecs();
        update_scene_dir_from_path(path.c_str());
        set_current_scene_path_internal(path.c_str());
        follow_scene_project_root(path.c_str());
        repair_scene_asset_paths();

        /* Persist as last-opened scene for next editor launch (mirrors the
         * synchronous path; the redundant-write guard is kept). */
        {
            JceEditorConfig _ecfg;
            if (jce_editor_config_load(&_ecfg)) {
                const bool path_changed =
                    strcmp(_ecfg.last_scene_path, path.c_str()) != 0;
                const bool was_front =
                    _ecfg.recent_scene_count > 0 &&
                    strcmp(_ecfg.recent_scene_paths[0], path.c_str()) == 0;
                snprintf(_ecfg.last_scene_path,
                         sizeof(_ecfg.last_scene_path), "%s", path.c_str());
                jce_editor_config_add_recent_scene(&_ecfg, path.c_str());
                if (path_changed || !was_front)
                    jce_editor_config_save(&_ecfg);
            }
        }
        mirror_last_scene_to_project(path.c_str());
        LOG_INFO(LOG_TAG, "scene loaded from %s (%d entities)",
                 path.c_str(), (int)g_entity_order.size());
    }

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

    /* Same per-scene camera restore as the synchronous path. */
    jce_editor_scene_camera_restore_pose(path.c_str());

    jce_editor_scene_render_streaming_autostart();
}

/* Tear down + reset the deferred-load record. */
void clear_deferred_scene_load()
{
    if (g_dsl.stream) {
        /* Should already be finalized by callers; guard against leaks if a
         * caller bails mid-stream. */
        (void)jce_scene_load_stream_finalize(g_dsl.stream);
        g_dsl.stream = nullptr;
    }
    if (g_dsl.root) {
        jce_json_free(g_dsl.root);
        g_dsl.root = nullptr;
    }
    g_dsl.active = false;
    g_dsl.total  = 0;
    g_dsl.scene_path.clear();
}

} /* anonymous namespace */

bool jce_state_load_scene_file_async(const char *scene_path)
{
    if (!scene_path || scene_path[0] == '\0')
        return false;

    /* If a previous async load is somehow still in flight, finish it
     * synchronously before swapping — never overlap two loads. */
    if (g_dsl.active) {
        if (g_dsl.stream) {
            jce_scene_load_stream_step(g_dsl.stream, /*all=*/0);
            jce_scene_load_stream_finalize(g_dsl.stream);
            g_dsl.stream = nullptr;
            finalize_deferred_scene_load();
        }
        clear_deferred_scene_load();
    }

    /* Same pre-swap teardown as the synchronous path. */
    jce_panel_sequencer_preview_flush();
    stop_play_before_scene_swap();
    jce_editor_scene_render_streaming_teardown();
    close_active_bundle_mount();

    /* Inform the engine parser of the scene's base directory so sibling
     * material backfill resolves (jce_scene_serial_load_file does this; we
     * bypass it, so set it ourselves). */
    {
        const char *sep = strrchr(scene_path, '/');
        const char *bs  = strrchr(scene_path, '\\');
        if (bs > sep) sep = bs;
        if (sep) {
            char dir[1024];
            size_t L = (size_t)(sep - scene_path);
            if (L >= sizeof(dir)) L = sizeof(dir) - 1;
            memcpy(dir, scene_path, L);
            dir[L] = '\0';
            jce_scene_serial_set_base_dir(dir);
        } else {
            jce_scene_serial_set_base_dir(nullptr);
        }
    }

    /* Parse the file (read + cJSON) before touching the live scene, so a
     * read/parse failure leaves the current scene intact. */
    JceJson *root = jce_json_parse_file(scene_path);
    if (!root) {
        LOG_WARN(LOG_TAG, "scene load failed (parse): %s", scene_path);
        return false;
    }

    /* Contract gate (mirrors jce_scene_serial_load). */
    int cmaj = (int)JCE_SCENE_CONTRACT_MAJOR;
    int cmin = (int)JCE_SCENE_CONTRACT_MINOR;
    parse_scene_contract_version(root, &cmaj, &cmin);
    if (!jce_scene_contract_major_compatible((uint32_t)cmaj)) {
        LOG_WARN(LOG_TAG,
                 "scene load failed: unsupported contract major %d (expected %u)",
                 cmaj, (unsigned)JCE_SCENE_CONTRACT_MAJOR);
        jce_json_free(root);
        return false;
    }

    /* Parse succeeded — now it is safe to drop the old scene. */
    {
        HistorySuspendScope suspend;
        clear_scene_entities();
    }

    /* Follow the NEW scene's asset-resolve base + project root BEFORE creating
     * its entities, so their mesh/material refs resolve against this scene's
     * roots.  The synchronous load paths already do this; this (async/frame-
     * sliced) path bypassed jce_scene_serial_load_file and was missing them, so
     * switching scenes with the editor kept open left the asset-cache scene_dir
     * + project root pointing at the PREVIOUS scene → the new scene's models
     * failed to resolve ("模型无法加载") until an editor restart. */
    update_scene_dir_from_path(scene_path);
    follow_scene_project_root(scene_path);

    /* Scene-switch cache reset (same rationale as the sync path): drop the
     * renderer/pick model caches + the negative resolve-miss cache so stale
     * failed-load flags from the previous scene cannot block the new scene's
     * models.  Without this, switching scenes with the editor open left models
     * failing to load until a restart. */
    jce_editor_scene_render_invalidate_model_caches();
    jce_editor_scene_asset_cache_clear_resolve_misses();

    /* Begin the incremental load on the now-empty scene. */
    int total = 0;
    JceSceneLoadStream *stream =
        jce_scene_load_stream_begin(s.scene, root, &total);
    if (!stream) {
        /* Empty/invalid entity array, or OOM.  Treat like an empty scene:
         * finalize editor state against whatever exists (nothing) so the
         * editor is in a clean, consistent state rather than half-torn. */
        jce_json_free(root);
        g_dsl.scene_path = scene_path;
        finalize_deferred_scene_load();
        clear_deferred_scene_load();
        LOG_WARN(LOG_TAG, "scene load: no entity data in %s", scene_path);
        return true;
    }

    /* Small scenes: finish in one shot (identical timing to the sync path)
     * so we never show a loading overlay for trivial loads. */
    if (total <= kAsyncEntityThreshold) {
        jce_scene_load_stream_step(stream, /*all=*/0);
        jce_scene_load_stream_finalize(stream);
        jce_json_free(root);
        g_dsl.scene_path = scene_path;
        finalize_deferred_scene_load();
        clear_deferred_scene_load();
        return true;
    }

    /* Large scene: defer the create across frames. */
    g_dsl.active     = true;
    g_dsl.root       = root;   /* keep alive until finalize */
    g_dsl.stream     = stream;
    g_dsl.total      = total;
    g_dsl.scene_path = scene_path;
    LOG_INFO(LOG_TAG, "scene open (deferred): %s — %d entities, time-sliced",
             scene_path, total);
    return true;
}

void jce_state_scene_load_poll(void)
{
    if (!g_dsl.active || !g_dsl.stream)
        return;

    /* Time-sliced entity creation.  We step small batches until a per-frame
     * wall-clock budget is spent rather than a fixed entity count: the first
     * batches pay the cold-cache cost (first read+parse of each unique mesh /
     * .mat.json), which a pure count budget cannot bound — a single 1500-entity
     * slice that happens to touch every unique material stalled the frame it
     * landed on.  A time budget caps any single frame's load cost and spreads
     * the rest across the next few frames, while the "Loading scene…" overlay
     * stays responsive.  Mirrors jce_world_streamer_update()'s apply budget and
     * honours the engine async doctrine (frame-slice; never block the main loop
     * on variable-latency disk work). */
    const uint64_t freq      = jce_time_perf_freq();
    const uint64_t t0        = jce_time_perf_counter();
    const double   budget_ms = 4.0;
    for (;;) {
        jce_scene_load_stream_step(g_dsl.stream, kEntitiesPerSlice);
        if (jce_scene_load_stream_done(g_dsl.stream))
            break;
        const double elapsed_ms =
            freq ? (double)(jce_time_perf_counter() - t0) / (double)freq * 1000.0
                 : budget_ms;
        if (elapsed_ms >= budget_ms)
            break;   /* resume next frame */
    }

    if (jce_scene_load_stream_done(g_dsl.stream)) {
        jce_scene_load_stream_finalize(g_dsl.stream);
        g_dsl.stream = nullptr;
        finalize_deferred_scene_load();
        clear_deferred_scene_load();
    }
}

bool jce_state_is_scene_loading(void)
{
    return g_dsl.active;
}

float jce_state_scene_load_progress(void)
{
    if (!g_dsl.active || g_dsl.total <= 0)
        return 0.0f;
    int processed = jce_scene_load_stream_processed(g_dsl.stream);
    float p = (float)processed / (float)g_dsl.total;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return p;
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
    const bool had_bundle = g_bm.fs != nullptr || g_bm.cat != nullptr ||
                            g_bm.bf != nullptr;

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

    if (had_bundle)
        jce_editor_scene_render_refresh_content_context();
}

bool apply_scene_bytes(const char *display_path,
                       const char *bytes, size_t size)
{
    /* Loading a scene (bundle / memory) during Play would swap the
     * JceScene out from under a running runtime — stop it first. */
    stop_play_before_scene_swap();

    /* Streamer holds entity handles into the old scene — destroy first. */
    jce_editor_scene_render_streaming_teardown();

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

        /* Show the streamed world in the editor scene view for streaming scenes
         * (auto-enables preview so the editor matches Play). */
        jce_editor_scene_render_streaming_autostart();
    }
    return ok;
}
} /* namespace */

void jce_state_close_bundle_preview(void)
{
    close_active_bundle_mount();
}

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
    jce_fs_set_active_policy(g_bm.fs, JCE_FS_ACTIVE_ISOLATED);
    jce_editor_scene_render_refresh_content_context();
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
    jce_fs_set_active_policy(g_bm.fs, JCE_FS_ACTIVE_ISOLATED);
    jce_editor_scene_render_refresh_content_context();
    bool ok = apply_scene_bytes(display, (const char *)raw, (size_t)sz);
    jce_fs_buffer_free(raw);
    if (!ok) close_active_bundle_mount();
    return ok;
}
