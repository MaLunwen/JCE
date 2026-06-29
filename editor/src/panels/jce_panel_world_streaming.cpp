/*
 * jce_panel_world_streaming.cpp — World-streaming authoring panel.
 *
 * Edits the scene-level JceSceneStreamingSettings block (streamer config
 * + explicit chunk registry) that jce_scene_components_json.c serializes
 * as the scene's "streaming" object.  All mutations go through
 * jce_scene_get_streaming_settings_mut() wrapped in begin/end_batch_edit,
 * so undo/redo comes for free via the scene-JSON snapshot history.
 *
 * The "Preview" toggle (session-local, default off) drives the editor's
 * live streamer: jce_editor_scene_render_streaming_rebuild() recreates
 * the JceWorldStreamer from the authored settings — the config struct has
 * no setters, so EVERY settings change recreates the streamer.  Preview
 * spawns/destroys real chunk entities in the hierarchy as the editor
 * camera moves; saving the scene is safe (the streamer is destroyed
 * before serialization — see jce_state_save_scene_file).
 */

#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"
#include "core/jce_assetdb.h"
#include "scene/jce_editor_scene_render.h"
#include "scene/jce_editor_scene_asset_cache.h"
#include "ui/jce_editor_panels.h"
#include "dialogs/jce_path_input.h"

#include <jce/tools/jce_imgui.hpp>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/resource/jce_world_streamer.h>
#include <jce/resource/jce_world_partition.h>
#include <jce/resource/jce_hlod_bake.h>
#include <jce/resource/jce_model_importer.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_math.h>
}

#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

/* Rebuild the preview streamer only once the active edit (drag) ends, so
 * radius scrubbing doesn't recreate the streamer every frame. */
bool g_rebuild_pending = false;

void request_rebuild(void)
{
    g_rebuild_pending = true;
}

void flush_pending_rebuild(void)
{
    if (!g_rebuild_pending) return;
    if (ImGui::IsAnyItemActive()) return;   /* wait for the drag to end */
    g_rebuild_pending = false;
    jce_editor_scene_render_streaming_rebuild();
}

/* Smallest id not used by any authored chunk. */
uint32_t next_free_chunk_id(const JceSceneStreamingSettings *st)
{
    uint32_t id = 0;
    for (;;) {
        bool taken = false;
        for (uint32_t i = 0; i < st->chunk_count; ++i) {
            if (st->chunks[i].id == id) { taken = true; break; }
        }
        if (!taken) return id;
        ++id;
    }
}

const char *chunk_state_label(JceChunkState state)
{
    switch (state) {
    case JCE_CHUNK_LOADED:    return jce_editor_i18n("panel.streaming.state.loaded");
    case JCE_CHUNK_LOADING:   return jce_editor_i18n("panel.streaming.state.loading");
    case JCE_CHUNK_UNLOADING: return jce_editor_i18n("panel.streaming.state.unloading");
    case JCE_CHUNK_UNLOADED:
    default:                  return jce_editor_i18n("panel.streaming.state.unloaded");
    }
}

ImVec4 chunk_state_color(JceChunkState state)
{
    switch (state) {
    case JCE_CHUNK_LOADED:    return ImVec4(0.35f, 0.90f, 0.35f, 1.0f);
    case JCE_CHUNK_LOADING:
    case JCE_CHUNK_UNLOADING: return ImVec4(0.95f, 0.85f, 0.30f, 1.0f);
    case JCE_CHUNK_UNLOADED:
    default:                  return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
    }
}

/* Commit a locally edited copy back into the scene inside one undo batch.
 * The undo system snapshots the full scene JSON (which now includes the
 * "streaming" block), so this is all the history wiring needed. */
void commit_settings(JceScene *scene, const JceSceneStreamingSettings &edited)
{
    jce_state_begin_batch_edit();
    JceSceneStreamingSettings *mut = jce_scene_get_streaming_settings_mut(scene);
    if (mut) *mut = edited;
    jce_state_end_batch_edit();
    request_rebuild();
}

/* ── Auto-partition (Direction A1) ──────────────────────────────────────
 * One click: spatial-hash the authored scene into the streaming roster +
 * per-cell fragment files the runtime already consumes (jce_world_partition).
 * The scene file is rewritten as the master (residents + roster); streamed
 * entities move into scenes/chunks/cell_<gx>_<gz>.scene.json.  This is the
 * engine equivalent of "author one world, the engine grids it". */
float s_part_cell_size     = 128.0f;   /* XZ grid cell size (m)              */
float s_part_load_radius   = 420.0f;
float s_part_unload_radius = 600.0f;
float s_part_resident_max  = 500.0f;   /* AABB extent above this stays resident
                                        * (ground / skybox / world-scale meshes) */
char  s_part_status[256]   = {0};

struct EntCollect {
    JcePartitionEntity *ents;
    uint32_t            count, cap;
    float               resident_max_extent;
};

void partition_collect_cb(JceScene *s, JceEntity e, void *ud)
{
    EntCollect *c = (EntCollect *)ud;
    if (c->count == c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 256;
        JcePartitionEntity *nb =
            (JcePartitionEntity *)realloc(c->ents, (size_t)nc * sizeof *nb);
        if (!nb) return;
        c->ents = nb; c->cap = nc;
    }
    JcePartitionEntity *pe = &c->ents[c->count++];
    memset(pe, 0, sizeof *pe);
    pe->entity_id = e;

    float mn[3], mx[3];
    if (jce_editor_scene_camera_get_entity_focus_bounds((uint32_t)e, mn, mx)) {
        pe->center[0] = (mn[0] + mx[0]) * 0.5f;
        pe->center[1] = (mn[1] + mx[1]) * 0.5f;
        pe->center[2] = (mn[2] + mx[2]) * 0.5f;
        float ex = mx[0]-mn[0], ey = mx[1]-mn[1], ez = mx[2]-mn[2];
        pe->radius = 0.5f * sqrtf(ex*ex + ey*ey + ez*ez);
        float maxext = ex; if (ey > maxext) maxext = ey; if (ez > maxext) maxext = ez;
        /* Stream world-content meshes that aren't huge.  A huge AABB == ground /
         * skybox → keep resident; non-mesh entities (lights/cameras/empties)
         * have no bounds → resident.  A per-entity "Keep Resident" tag can
         * refine this later. */
        pe->streamable = jce_scene_has_mesh_renderer(s, e) &&
                         (maxext <= c->resident_max_extent);
    } else {
        pe->streamable = false;
    }
}

void path_dir_into(const char *path, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s", path ? path : "");
    char *sl = strrchr(out, '/');
    char *bs = strrchr(out, '\\');
    if (bs > sl) sl = bs;
    if (sl) *sl = '\0';
}

/* Case-insensitive (Windows-safe) "is `dir` under `root`?" with a path-boundary
 * check (so D:/proj_other is NOT treated as under D:/proj).  On success *out_rel
 * points to the in-`dir` remainder with leading slashes stripped. */
bool path_under_root(const char *dir, const char *root, size_t rootlen,
                     const char **out_rel)
{
    if (rootlen == 0) return false;
    for (size_t i = 0; i < rootlen; ++i) {
        char a = dir[i], b = root[i];
        if (a == '\0') return false;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    if (dir[rootlen] != '/' && dir[rootlen] != '\0') return false;
    const char *rel = dir + rootlen;
    while (*rel == '/') ++rel;
    *out_rel = rel;
    return true;
}

/* Append an HLOD_<gx>_<gz> proxy entity (Transform@origin + MeshRenderer->glb +
 * EditorMeta name) to the master scene's entities array — matches exactly what
 * jce_world_streamer_attach_hlod scans for (visibility toggled by cell state). */
void add_hlod_proxy_entity(JceJson *entities, uint32_t id, int gx, int gz,
                           const char *glb_vfs)
{
    char name[64];
    snprintf(name, sizeof name, "HLOD_%d_%d", gx, gz);

    JceJson *e = jce_json_object();
    jce_json_set_number(e, "id", (double)id);
    jce_json_set_string(e, "name", name);
    jce_json_set_int(e, "parentId", 0);
    JceJson *comps = jce_json_array();

    JceJson *tf = jce_json_object();
    jce_json_set_string(tf, "type", "Transform");
    jce_json_set_number(tf, "posX", 0.0); jce_json_set_number(tf, "posY", 0.0); jce_json_set_number(tf, "posZ", 0.0);
    jce_json_set_number(tf, "rotX", 0.0); jce_json_set_number(tf, "rotY", 0.0); jce_json_set_number(tf, "rotZ", 0.0);
    jce_json_set_number(tf, "scaleX", 1.0); jce_json_set_number(tf, "scaleY", 1.0); jce_json_set_number(tf, "scaleZ", 1.0);
    jce_json_array_push(comps, tf);

    JceJson *mr = jce_json_object();
    jce_json_set_string(mr, "type", "MeshRenderer");
    jce_json_set_string(mr, "meshPath", glb_vfs);
    jce_json_set_number(mr, "baseColorR", 1.0); jce_json_set_number(mr, "baseColorG", 1.0);
    jce_json_set_number(mr, "baseColorB", 1.0); jce_json_set_number(mr, "baseColorA", 1.0);
    jce_json_set_bool(mr, "doubleSided", true);
    jce_json_set_bool(mr, "castsShadow", false);
    jce_json_set_bool(mr, "receivesShadow", true);
    jce_json_array_push(comps, mr);

    JceJson *em = jce_json_object();
    jce_json_set_string(em, "type", "EditorMeta");
    jce_json_set_string(em, "name", name);
    jce_json_array_push(comps, em);

    jce_json_set_child(e, "components", comps);
    jce_json_array_push(entities, e);
}

/* ── Incremental HLOD bake: per-cell fingerprint cache (large-world #8b) ──────
 * Hash a cell's bake INPUTS (entity ids + world transforms + mesh paths) into a
 * 64-bit fingerprint stored next to its proxy .glb.  On the next Partition World
 * a cell whose fingerprint is unchanged + whose proxy still exists skips the
 * expensive mesh-load + simplify entirely (it just re-injects the existing
 * proxy).  Without this, every Partition World re-baked ALL cells, making a
 * multi-km world impractical to iterate. */
static inline uint64_t hlod_fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

static uint64_t hlod_cell_fingerprint(JceScene *scene, const JcePartitionEntity *ents,
                                      uint32_t ent_count, const JcePartitionConfig *cfg,
                                      int gx, int gz, uint32_t *out_n)
{
    uint64_t h = 1469598103934665603ULL;            /* FNV-1a 64 offset basis */
    h = hlod_fnv(h, &gx, sizeof gx); h = hlod_fnv(h, &gz, sizeof gz);
    uint32_t n = 0;
    for (uint32_t i = 0; i < ent_count; ++i) {
        if (!ents[i].streamable) continue;
        int egx = (int)floorf(ents[i].center[0] / cfg->cell_size);
        int egz = (int)floorf(ents[i].center[2] / cfg->cell_size);
        if (egx != gx || egz != gz) continue;
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, (JceEntity)ents[i].entity_id);
        if (!mr || mr->mesh_path[0] == '\0') continue;
        uint64_t id = (uint64_t)ents[i].entity_id;
        h = hlod_fnv(h, &id, sizeof id);
        jce_mat4 w = jce_scene_get_world_matrix(scene, (JceEntity)ents[i].entity_id);
        h = hlod_fnv(h, &w, sizeof w);
        h = hlod_fnv(h, mr->mesh_path, strlen(mr->mesh_path));
        ++n;
    }
    if (out_n) *out_n = n;
    return h;
}

static uint64_t hlod_read_fp(const char *path)
{
    uint64_t size = 0, v = 0;
    void *d = jce_fs_host_read_all(path, &size);
    if (d) { if (size >= sizeof v) memcpy(&v, d, sizeof v); jce_fs_buffer_free(d); }
    return v;   /* 0 (no/short file) never matches a real fingerprint -> bakes */
}

/* For each cell, gather its streamable entities' CPU meshes + world matrices,
 * bake a merged+simplified proxy .glb (jce_hlod_bake_proxy), and inject the HLOD
 * proxy entity into the master.  Re-derives the cell of each entity with the same
 * spatial hash the partitioner used.  Unchanged cells (fingerprint match + proxy
 * present) are reused, not re-baked.  Returns total proxies (baked + reused);
 * *out_reused (may be NULL) receives the reused count. */
uint32_t bake_hlods(JceScene *scene, const JcePartitionEntity *ents, uint32_t ent_count,
                    const JcePartitionConfig *cfg, JcePartitionResult *res,
                    const char *root_norm, uint32_t *out_reused)
{
    JceJson *msc  = jce_json_get(res->master_json, "scene");
    JceJson *ment = msc ? jce_json_get(msc, "entities") : NULL;
    if (!ment) return 0;

    char hlod_dir[1280];
    snprintf(hlod_dir, sizeof hlod_dir, "%s/models/hlod", root_norm);
    jce_fs_host_create_directory(hlod_dir);

    jce_scene_invalidate_world_cache(scene);

    uint32_t baked = 0, reused = 0;
    for (uint32_t c = 0; c < res->chunk_count; ++c) {
        const int gx = res->chunks[c].gx, gz = res->chunks[c].gz;

        char glb_vfs[256], glb_host[1400], fp_host[1408];
        snprintf(glb_vfs,  sizeof glb_vfs,  "models/hlod/chunk_%d_%d.glb", gx, gz);
        snprintf(glb_host, sizeof glb_host, "%s/%s", root_norm, glb_vfs);
        snprintf(fp_host,  sizeof fp_host,  "%s/models/hlod/chunk_%d_%d.fp",
                 root_norm, gx, gz);

        /* Incremental: unchanged inputs + an existing proxy => reuse, skip the
         * expensive re-bake (mesh-load + simplify). */
        uint32_t cell_n = 0;
        uint64_t fp = hlod_cell_fingerprint(scene, ents, ent_count, cfg, gx, gz, &cell_n);
        if (cell_n == 0) continue;
        if (jce_fs_host_exists_file(glb_host) && hlod_read_fp(fp_host) == fp) {
            add_hlod_proxy_entity(ment, 9000000u + res->chunks[c].id, gx, gz, glb_vfs);
            ++reused;
            continue;
        }

        JceHlodMeshInput    *inputs = NULL;
        JceModelCpuMeshData *mds    = NULL;
        uint32_t n = 0, cap = 0;

        for (uint32_t i = 0; i < ent_count; ++i) {
            if (!ents[i].streamable) continue;
            int egx = (int)floorf(ents[i].center[0] / cfg->cell_size);
            int egz = (int)floorf(ents[i].center[2] / cfg->cell_size);
            if (egx != gx || egz != gz) continue;

            JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, (JceEntity)ents[i].entity_id);
            if (!mr || mr->mesh_path[0] == '\0') continue;
            char host[1024];
            if (!jce_editor_scene_asset_cache_resolve_mesh_path(mr->mesh_path, host, (int)sizeof host))
                continue;
            JceModelCpuMeshData md; memset(&md, 0, sizeof md);
            if (!jce_model_importer_load_cpu_file(host, &md)) continue;
            if (md.vertex_count == 0 || md.index_count < 3) { jce_model_importer_free_cpu(&md); continue; }

            if (n == cap) {
                uint32_t ncap = cap ? cap * 2 : 16;
                void *ni = realloc(inputs, ncap * sizeof *inputs);
                void *nm = realloc(mds,    ncap * sizeof *mds);
                if (ni) inputs = (JceHlodMeshInput *)ni;     /* realloc may move */
                if (nm) mds    = (JceModelCpuMeshData *)nm;
                if (!ni || !nm) {   /* OOM: drop this mesh, bake what we have */
                    jce_model_importer_free_cpu(&md);
                    break;
                }
                cap = ncap;
            }
            jce_mat4 w = jce_scene_get_world_matrix(scene, (JceEntity)ents[i].entity_id);
            JceHlodMeshInput *in = &inputs[n];
            memset(in, 0, sizeof *in);
            in->positions       = (const char *)md.vertices + offsetof(JceMeshVertex, pos);
            in->position_stride = (uint32_t)sizeof(JceMeshVertex);
            in->normals         = (const char *)md.vertices + offsetof(JceMeshVertex, normal);
            in->normal_stride   = (uint32_t)sizeof(JceMeshVertex);
            in->vertex_count    = md.vertex_count;
            in->indices         = md.indices;
            in->index_count     = md.index_count;
            memcpy(in->world, &w, sizeof in->world);
            mds[n] = md;
            ++n;
        }

        if (n > 0) {
            float col[4] = { 0.62f, 0.64f, 0.66f, 1.0f };
            JceHlodBakeStats st;
            if (jce_hlod_bake_proxy(inputs, n, 0.15f, col, glb_host, &st)) {
                jce_fs_host_write_all(fp_host, &fp, sizeof fp);   /* cache the inputs */
                add_hlod_proxy_entity(ment, 9000000u + res->chunks[c].id, gx, gz, glb_vfs);
                ++baked;
            }
        }
        for (uint32_t i = 0; i < n; ++i) jce_model_importer_free_cpu(&mds[i]);
        free(inputs); free(mds);
    }
    if (out_reused) *out_reused = reused;
    return baked + reused;
}

/* Returns true iff it rewrote + reloaded the scene (so the caller must stop
 * using the now-destroyed `scene` pointer this frame). */
bool run_partition(JceScene *scene)
{
    const char *scene_path = jce_state_get_current_scene_path();
    if (!scene_path || !scene_path[0]) {
        snprintf(s_part_status, sizeof s_part_status, "Save the scene to a file first.");
        return false;
    }
    const char *root = jce_assetdb_get_root();
    if (!root || !root[0]) {
        snprintf(s_part_status, sizeof s_part_status, "No project root set.");
        return false;
    }

    /* Scene dir relative to the project root → VFS prefix for fragments
     * (the streamer mounts the project root). */
    char scene_dir[1024]; path_dir_into(scene_path, scene_dir, sizeof scene_dir);
    for (char *p = scene_dir; *p; ++p) if (*p == '\\') *p = '/';
    char root_norm[1024]; snprintf(root_norm, sizeof root_norm, "%s", root);
    for (char *p = root_norm; *p; ++p) if (*p == '\\') *p = '/';
    size_t rl = strlen(root_norm);
    while (rl > 0 && root_norm[rl-1] == '/') root_norm[--rl] = '\0';
    const char *rel = NULL;
    if (!path_under_root(scene_dir, root_norm, rl, &rel)) {
        snprintf(s_part_status, sizeof s_part_status,
                 "Scene must live under the project root to partition.");
        return false;
    }
    if (strlen(rel) + 40 > 250) {   /* engine chunk.path is char[256] */
        snprintf(s_part_status, sizeof s_part_status,
                 "Scene folder path too deep to partition.");
        return false;
    }

    char fmt[1280];
    if (rel[0]) snprintf(fmt, sizeof fmt, "%s/chunks/cell_%%d_%%d.scene.json", rel);
    else        snprintf(fmt, sizeof fmt, "chunks/cell_%%d_%%d.scene.json");

    EntCollect col; memset(&col, 0, sizeof col);
    col.resident_max_extent = s_part_resident_max;
    jce_scene_each_entity(scene, partition_collect_cb, &col);

    JcePartitionConfig cfg;
    cfg.cell_size         = s_part_cell_size;
    cfg.load_radius       = s_part_load_radius;
    cfg.unload_radius     = s_part_unload_radius;
    cfg.id_base           = 0;
    cfg.fragment_path_fmt = fmt;

    JcePartitionResult res;
    bool ok = jce_world_partition_build(scene, col.ents, col.count, &cfg, &res);
    if (!ok) {
        free(col.ents);
        snprintf(s_part_status, sizeof s_part_status,
                 "Partition failed (too many cells? raise the cell size).");
        return false;
    }
    /* Bake an HLOD proxy per cell (merged + simplified real geometry) and inject
     * HLOD_<gx>_<gz> proxies into the master.  Uses col.ents + the live scene, so
     * it runs before col.ents is freed and before the master is written. */
    uint32_t reused_hlods = 0;
    uint32_t hlods = bake_hlods(scene, col.ents, col.count, &cfg, &res, root_norm,
                                &reused_hlods);
    free(col.ents);

    /* Ensure the fragment directory exists, then write fragments + master. */
    char chunks_dir[1400];
    if (rel[0]) snprintf(chunks_dir, sizeof chunks_dir, "%s/%s/chunks", root_norm, rel);
    else        snprintf(chunks_dir, sizeof chunks_dir, "%s/chunks", root_norm);
    jce_fs_host_create_directory(chunks_dir);

    bool wok = true;
    for (uint32_t i = 0; i < res.chunk_count; ++i) {
        char host[1536];
        snprintf(host, sizeof host, "%s/%s", root_norm, res.chunks[i].path);
        if (!jce_json_write_file(host, res.chunks[i].json, true, false)) wok = false;
    }
    /* Write the master ONLY after every fragment succeeded — never clobber the
     * source scene into a master that references fragments that failed to write. */
    if (wok && !jce_json_write_file(scene_path, res.master_json, true, false))
        wok = false;

    uint32_t nch = res.chunk_count, ns = res.streamed_count, nr = res.resident_count;
    jce_world_partition_free(&res);

    if (!wok) {
        snprintf(s_part_status, sizeof s_part_status,
                 "Write failed before the scene file was touched (nothing clobbered).");
        return false;
    }

    /* Reload the rewritten master + turn streaming preview on. */
    jce_state_load_scene_file(scene_path);
    jce_state_set_streaming_preview(true);
    jce_editor_scene_render_streaming_rebuild();
    snprintf(s_part_status, sizeof s_part_status,
             "Partitioned: %u chunks, %u streamed, %u resident, %u HLOD proxies "
             "(%u reused).",
             nch, ns, nr, hlods, reused_hlods);
    return true;
}

bool draw_partition_section(JceScene *scene)
{
    bool reloaded = false;
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n_or("panel.streaming.section.partition",
                               "Auto-Partition (engine)")))
        return reloaded;

    ImGui::TextWrapped("%s", jce_editor_i18n_or(
        "panel.streaming.partition.help",
        "Spatial-hash this authored scene into the streaming roster + per-cell "
        "fragment files the runtime already streams. The scene file is rewritten "
        "as the master (residents + roster); streamed entities move into "
        "scenes/chunks/. Commit your work first so you can revert."));

    ImGui::DragFloat(jce_editor_i18n_id("panel.streaming.cellSize", "ws_pcell"), &s_part_cell_size, 1.0f,
                     8.0f, 100000.0f, "cell %.0f m");
    if (s_part_cell_size < 8.0f) s_part_cell_size = 8.0f;
    ImGui::DragFloat(jce_editor_i18n_id("panel.streaming.loadRadius", "ws_pload"), &s_part_load_radius, 1.0f,
                     1.0f, 100000.0f, "load %.0f m");
    ImGui::DragFloat(jce_editor_i18n_id("panel.streaming.unloadRadius", "ws_punload"), &s_part_unload_radius, 1.0f,
                     1.0f, 100000.0f, "unload %.0f m");
    if (s_part_unload_radius < s_part_load_radius)
        s_part_unload_radius = s_part_load_radius;
    ImGui::DragFloat(jce_editor_i18n_id("panel.streaming.residentIfLarger", "ws_presmax"), &s_part_resident_max,
                     1.0f, 1.0f, 1000000.0f, "resident if > %.0f m");

    if (ImGui::Button(jce_editor_i18n_id("panel.streaming.partitionWorld", "ws_part_run")))
        ImGui::OpenPopup("ws_partition_confirm");

    if (ImGui::BeginPopupModal("ws_partition_confirm", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", jce_editor_i18n_or(
            "panel.streaming.partition.confirm",
            "Rewrite this scene into a streaming master + per-cell fragments?\n"
            "This overwrites the scene file and writes scenes/chunks/*.json.\n"
            "Make sure your work is committed so you can revert."));
        ImGui::Separator();
        if (ImGui::Button(jce_editor_i18n_id("panel.streaming.partitionGo", "ws_part_go"))) {
            reloaded = run_partition(scene);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n_or("common.cancel", "Cancel")))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (s_part_status[0]) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", s_part_status);
    }
    return reloaded;
}

void draw_config(JceScene *scene, JceSceneStreamingSettings &st)
{
    bool changed = false;

    changed |= ImGui::Checkbox(
        jce_editor_i18n_id("panel.streaming.enable", "ws_enable"),
        &st.enabled);

    {
        const char *modes[2] = {
            jce_editor_i18n("panel.streaming.mode.radial"),
            jce_editor_i18n("panel.streaming.mode.rectangular"),
        };
        int mode = (st.mode == 1) ? 1 : 0;
        if (ImGui::Combo(jce_editor_i18n_id("panel.streaming.mode", "ws_mode"),
                         &mode, modes, 2)) {
            st.mode = mode;
            changed = true;
        }
    }

    changed |= ImGui::DragFloat(
        jce_editor_i18n_id("panel.streaming.loadRadius", "ws_loadr"),
        &st.load_radius, 1.0f, 1.0f, 100000.0f, "%.0f m");
    changed |= ImGui::DragFloat(
        jce_editor_i18n_id("panel.streaming.unloadRadius", "ws_unloadr"),
        &st.unload_radius, 1.0f, 1.0f, 100000.0f, "%.0f m");
    /* Authoring invariant: unload must not undercut load (the streamer
     * would thrash load/unload at the boundary). */
    if (st.load_radius < 1.0f) st.load_radius = 1.0f;
    if (st.unload_radius < st.load_radius) st.unload_radius = st.load_radius;

    {
        int budget = (int)st.budget_mb;
        if (ImGui::DragInt(
                jce_editor_i18n_id("panel.streaming.budgetMb", "ws_budget"),
                &budget, 1.0f, 0, 16384, "%d MiB")) {
            st.budget_mb = (budget < 0) ? 0u : (uint32_t)budget;
            changed = true;
        }
        int pending = (int)st.max_pending;
        if (ImGui::DragInt(
                jce_editor_i18n_id("panel.streaming.maxPending", "ws_pending"),
                &pending, 0.1f, 1, 64)) {
            st.max_pending = (pending < 1) ? 1u : (uint32_t)pending;
            changed = true;
        }
        changed |= ImGui::DragFloat(
            jce_editor_i18n_id("panel.streaming.frameBudgetMs", "ws_framems"),
            &st.frame_budget_ms, 0.1f, 0.1f, 16.0f, "%.1f ms");
        if (st.frame_budget_ms < 0.1f) st.frame_budget_ms = 0.1f;
    }

    if (changed)
        commit_settings(scene, st);
}

void draw_chunk_table(JceScene *scene, JceSceneStreamingSettings &st,
                      JceWorldStreamer *ws)
{
    bool     changed    = false;
    int      delete_idx = -1;
    const bool previewing = (ws != NULL);
    /* The "render" checkbox column is only meaningful while previewing in
     * FILTER mode (it drives the per-chunk filter set / hierarchy sync). */
    const bool filtering =
        previewing &&
        jce_state_streaming_get_preview_mode() == JCE_STREAM_PREVIEW_FILTER;

    const int cols = (previewing ? 6 : 5) + (filtering ? 1 : 0);
    if (ImGui::BeginTable("ws_chunks", cols,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerH)) {
        if (filtering)
            ImGui::TableSetupColumn(
                jce_editor_i18n_or("panel.streaming.col.render", "Show"),
                ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.streaming.col.id"),
                                ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.streaming.col.center"));
        ImGui::TableSetupColumn(jce_editor_i18n("panel.streaming.col.radius"),
                                ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.streaming.col.path"));
        if (previewing)
            ImGui::TableSetupColumn(
                jce_editor_i18n("panel.streaming.col.state"),
                ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.streaming.col.actions"),
                                ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();

        for (uint32_t i = 0; i < st.chunk_count; ++i) {
            JceSceneStreamChunk &c = st.chunks[i];
            ImGui::TableNextRow();
            ImGui::PushID((int)i);

            if (filtering) {
                ImGui::TableNextColumn();
                bool shown = jce_state_streaming_filter_contains(c.id);
                if (ImGui::Checkbox("##ws_render", &shown))
                    jce_state_streaming_filter_set(c.id, shown);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%u", c.id);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::DragFloat3("##ws_center", c.center, 0.5f);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::DragFloat("##ws_radius", &c.radius,
                                        0.5f, 0.0f, 100000.0f, "%.0f");
            if (c.radius < 0.0f) c.radius = 0.0f;

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            {
                /* Project-relative scene-fragment path (same base the
                 * streamer FS mounts — the project root). */
                JcePathInputOpts opts;
                opts.title = jce_editor_i18n("panel.streaming.col.path");
                changed |= jce_draw_path_input("##ws_path", c.path,
                                               sizeof(c.path),
                                               JcePathKind::AssetVfs, &opts);
            }

            if (previewing) {
                ImGui::TableNextColumn();
                JceChunkState cs = jce_world_streamer_chunk_state(ws, c.id);
                ImGui::TextColored(chunk_state_color(cs), "%s",
                                   chunk_state_label(cs));
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton(
                    jce_editor_i18n_id("panel.streaming.useCamera", "ws_cam"))) {
                JceCamera *cam = jce_editor_scene_get_camera();
                if (cam) {
                    jce_vec3 eye = jce_camera_get_position(cam);
                    c.center[0] = eye.x;
                    c.center[1] = eye.y;
                    c.center[2] = eye.z;
                    changed = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(
                    jce_editor_i18n_id("panel.streaming.delete", "ws_del")))
                delete_idx = (int)i;

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (delete_idx >= 0 && (uint32_t)delete_idx < st.chunk_count) {
        for (uint32_t i = (uint32_t)delete_idx; i + 1 < st.chunk_count; ++i)
            st.chunks[i] = st.chunks[i + 1];
        st.chunk_count--;
        memset(&st.chunks[st.chunk_count], 0, sizeof(st.chunks[0]));
        changed = true;
    }

    /* Add row — refuse chunk #257 (the streamer's roster pool is fixed at
     * JCE_SCENE_MAX_STREAM_CHUNKS; extra chunks would be silently inert). */
    if (st.chunk_count >= JCE_SCENE_MAX_STREAM_CHUNKS) {
        ImGui::TextDisabled("%s", jce_editor_i18n("panel.streaming.chunkLimit"));
    } else if (ImGui::Button(
                   jce_editor_i18n_id("panel.streaming.addChunk", "ws_add"))) {
        JceSceneStreamChunk &c = st.chunks[st.chunk_count];
        memset(&c, 0, sizeof(c));
        c.id     = next_free_chunk_id(&st);
        c.radius = 50.0f;
        JceCamera *cam = jce_editor_scene_get_camera();
        if (cam) {
            jce_vec3 eye = jce_camera_get_position(cam);
            c.center[0] = eye.x;
            c.center[1] = eye.y;
            c.center[2] = eye.z;
        }
        st.chunk_count++;
        changed = true;
    }

    if (changed)
        commit_settings(scene, st);
}

/* ── Preview section: Full-World / Filter mode selector + helpers ─────
 * Drives jce_state_streaming_* (the session-local SSOT shared with the
 * Hierarchy panel) which forwards to jce_world_streamer_set_preview_load. */
void draw_preview_section(JceSceneStreamingSettings &st)
{
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n_or("panel.streaming.section.preview", "Preview"),
            ImGuiTreeNodeFlags_DefaultOpen))
        return;

    /* The mode selector only does anything once the live preview streamer is
     * up.  Switching to Full-World / Filtered auto-enables the preview toggle
     * (and rebuilds the streamer) if it was off, so the user doesn't have to
     * flip two switches. */
    JceStreamPreviewMode mode = jce_state_streaming_get_preview_mode();
    int sel = (int)mode;

    const char *modes[3] = {
        jce_editor_i18n_or("panel.streaming.preview.radius",    "Radius (around focus)"),
        jce_editor_i18n_or("panel.streaming.preview.all",       "Full World (all chunks)"),
        jce_editor_i18n_or("panel.streaming.preview.filtered",  "Filtered (chosen chunks)"),
    };
    if (ImGui::Combo(jce_editor_i18n_id("panel.streaming.preview.mode", "ws_pmode"),
                     &sel, modes, 3)) {
        JceStreamPreviewMode new_mode = (JceStreamPreviewMode)sel;
        /* Auto-enable the streaming preview when leaving Radius so the mode has
         * a live streamer to act on. */
        if (new_mode != JCE_STREAM_PREVIEW_RADIUS &&
            !jce_state_get_streaming_preview()) {
            jce_state_set_streaming_preview(true);
            jce_editor_scene_render_streaming_rebuild();
        }
        jce_state_streaming_set_preview_mode(new_mode);
    }

    /* Loaded / total chunk counts. */
    JceWorldStreamer *ws = jce_state_get_streaming_preview()
                         ? jce_editor_get_world_streamer() : NULL;
    uint32_t loaded = ws ? jce_world_streamer_loaded_count(ws) : 0;
    uint32_t pending = ws ? jce_world_streamer_pending_count(ws) : 0;
    uint32_t total  = st.chunk_count;
    uint32_t ents   = ws ? jce_world_streamer_entity_count(ws) : 0;
    ImGui::Text("%s", jce_editor_i18n_or("panel.streaming.preview.counts",
                                         "Chunks loaded:"));
    ImGui::SameLine();
    ImGui::Text("%u / %u  (%u pending, %u entities)", loaded, total, pending, ents);

    /* ── Residency / VRAM (large-world-opt VRAM ceiling) ──────────────────
     * Estimated residency = the streaming system's budget accounting (real GPU
     * bytes via the residency query in the runtime; per-entity estimate as the
     * floor).  The renderer's model-VRAM figure + cache size + eviction count
     * surface that freeing entities on cell unload actually frees GPU memory.
     * In the editor the asset cache (not this renderer) owns model GPU memory,
     * so the renderer's model-VRAM reads 0 here — labelled accordingly. */
    if (ws) {
        const double MB = 1024.0 * 1024.0;
        uint64_t used = jce_world_streamer_memory_used(ws);
        uint32_t budget_mb = st.budget_mb;
        if (budget_mb > 0)
            ImGui::Text(jce_editor_i18n("panel.streaming.residencyBudgetFmt"),
                        (double)used / MB, budget_mb);
        else
            ImGui::Text(jce_editor_i18n("panel.streaming.residencyUnlimitedFmt"),
                        (double)used / MB);

        uint32_t evicted = jce_world_streamer_evicted_count(ws);
        ImGui::Text(jce_editor_i18n("panel.streaming.lruEvictionsFmt"), evicted);

        JceSceneRenderer *sr = jce_editor_get_scene_renderer();
        if (sr) {
            uint64_t model_vram = jce_scene_renderer_model_vram_bytes(sr);
            uint32_t model_count = jce_scene_renderer_model_cache_count(sr);
            uint32_t model_evicted = jce_scene_renderer_model_evicted_count(sr);
            ImGui::Text(jce_editor_i18n("panel.streaming.modelCacheFmt"),
                        model_count, (double)model_vram / MB, model_evicted);
            if (model_vram == 0 && model_count > 0)
                ImGui::TextDisabled("%s", jce_editor_i18n("panel.streaming.modelCacheNote"));
        }
    }

    if (mode == JCE_STREAM_PREVIEW_ALL) {
        ImGui::TextWrapped("%s", jce_editor_i18n_or(
            "panel.streaming.preview.allHint",
            "Full World loads the whole city (~all chunks / tens of thousands of "
            "entities). Heavier — it streams in over a few seconds."));
    } else if (mode == JCE_STREAM_PREVIEW_FILTER) {
        ImGui::TextWrapped("%s", jce_editor_i18n_or(
            "panel.streaming.preview.filterHint",
            "Only ticked chunks render. Use the 'Show' checkboxes in the chunk "
            "list below (or the per-chunk eye in the Hierarchy) to choose."));
        ImGui::Text("%s %u",
                    jce_editor_i18n_or("panel.streaming.preview.selected", "Selected:"),
                    jce_state_streaming_filter_count());

        /* Filter-set helpers: Select All / None / In View. */
        if (ImGui::SmallButton(
                jce_editor_i18n_id("panel.streaming.preview.selectAll", "ws_selall"))) {
            uint32_t ids[JCE_SCENE_MAX_STREAM_CHUNKS];
            uint32_t n = 0;
            for (uint32_t i = 0; i < st.chunk_count &&
                                 n < JCE_SCENE_MAX_STREAM_CHUNKS; ++i)
                if (st.chunks[i].path[0] != '\0') ids[n++] = st.chunks[i].id;
            jce_state_streaming_filter_set_all(ids, n);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(
                jce_editor_i18n_id("panel.streaming.preview.selectNone", "ws_selnone")))
            jce_state_streaming_filter_clear();
        ImGui::SameLine();
        if (ImGui::SmallButton(
                jce_editor_i18n_id("panel.streaming.preview.selectView", "ws_selview"))) {
            /* "In current view" ≈ chunks whose centre is within the unload
             * radius of the orbit target (what the camera is looking at). */
            float tgt[3] = { 0, 0, 0 };
            jce_editor_scene_camera_get_target(tgt);
            float r = st.unload_radius > 0.0f ? st.unload_radius : 1.0f;
            float r2 = r * r;
            uint32_t ids[JCE_SCENE_MAX_STREAM_CHUNKS];
            uint32_t n = 0;
            for (uint32_t i = 0; i < st.chunk_count &&
                                 n < JCE_SCENE_MAX_STREAM_CHUNKS; ++i) {
                if (st.chunks[i].path[0] == '\0') continue;
                float dx = st.chunks[i].center[0] - tgt[0];
                float dy = st.chunks[i].center[1] - tgt[1];
                float dz = st.chunks[i].center[2] - tgt[2];
                if (dx*dx + dy*dy + dz*dz <= r2) ids[n++] = st.chunks[i].id;
            }
            jce_state_streaming_filter_set_all(ids, n);
        }
    }
}

void draw_content(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) {
        ImGui::TextDisabled("%s", jce_editor_i18n("panel.streaming.noScene"));
        return;
    }

    flush_pending_rebuild();

    /* Edit a local copy; commit_settings() writes it back through the
     * _mut accessor inside one undo batch when something changed.  The
     * block is ~70 KB — static keeps it off the ImGui draw stack. */
    static JceSceneStreamingSettings s_edit;
    const JceSceneStreamingSettings *cur = jce_scene_get_streaming_settings(scene);
    s_edit = cur ? *cur : jce_scene_streaming_settings_default();

    /* Session preview toggle — NOT part of the scene data / undo history. */
    {
        bool preview = jce_state_get_streaming_preview();
        if (ImGui::Checkbox(
                jce_editor_i18n_id("panel.streaming.preview", "ws_preview"),
                &preview)) {
            jce_state_set_streaming_preview(preview);
            jce_editor_scene_render_streaming_rebuild();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", jce_editor_i18n("panel.streaming.help"));
    }
    ImGui::Separator();

    /* Partitioning reloads the scene (destroys `scene`); bail this frame so we
     * never touch the dangling pointer — next frame redraws the fresh scene. */
    if (draw_partition_section(scene))
        return;
    ImGui::Separator();

    draw_preview_section(s_edit);
    ImGui::Separator();

    if (ImGui::CollapsingHeader(jce_editor_i18n("panel.streaming.section.config"),
                                ImGuiTreeNodeFlags_DefaultOpen))
        draw_config(scene, s_edit);

    ImGui::Separator();

    if (ImGui::CollapsingHeader(jce_editor_i18n("panel.streaming.section.chunks"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        JceWorldStreamer *ws = jce_state_get_streaming_preview()
                             ? jce_editor_get_world_streamer() : NULL;
        draw_chunk_table(scene, s_edit, ws);
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", jce_editor_i18n("panel.streaming.help"));
}

} /* namespace */

extern "C" void jce_editor_panel_world_streaming(void)
{
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###world_streaming",
             jce_editor_i18n("window.worldStreaming"));
    if (!ImGui::Begin(_wt,
                      jce_editor_panel_visible_ptr(JCE_PANEL_WORLD_STREAMING),
                      ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }
    draw_content();
    ImGui::End();
}
