/*
 * jce_panel_navmesh.cpp -- NavMesh authoring + bake (P1-navmesh-chain).
 *
 * The Bake button drives the real Recast/Detour backend
 * (jce_recast_build_to_file): it gathers world-space triangle soup from
 * the active scene (MeshRenderer CPU meshes transformed into world
 * space, plus terrain ground planes), runs the full voxelisation
 * pipeline and serialises the single-tile Detour navmesh to a
 * .navmesh.bin sidecar.  The runtime loads that file via
 * jce_recast_load_file and resolves agent paths through
 * jce_recast_find_path.
 *
 * Bake settings are persisted to a .navmesh.json companion (same
 * basename) so re-bakes are reproducible; the .json carries no grid
 * data anymore.
 */

#include "io/jce_editor_file_util.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_project_state.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_theme_palette.h"
#include "ui/jce_editor_colors.h"
#include "scene/jce_editor_scene_render.h"
#include "scene/jce_model_loader_assimp.h"

#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_thread.h>
#include <jce/api_scene.h>
#include <jce/middleware/ai/jce_navmesh_recast.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct BakeSettings {
    float cell_size      = 0.30f;
    float cell_height    = 0.20f;
    float agent_radius   = 0.40f;
    float agent_height   = 1.80f;
    float max_slope_deg  = 45.0f;
    float climb_step     = 0.40f;
    float bounds_min[3]  = { -32.0f, -2.0f, -32.0f };
    float bounds_max[3]  = {  32.0f, 12.0f,  32.0f };
};

struct BakeResult {
    JceRecastStats stats = {};       /* from the last successful bake */
    uint32_t       in_vertices  = 0; /* triangle-soup verts fed to Recast */
    uint32_t       in_triangles = 0;
    char           out_path[300] = { 0 }; /* the .navmesh.bin that was written */
};

struct State {
    BakeSettings cfg;
    BakeResult   result;
    bool         have_result = false;
    char         path[260] = "untitled.navmesh.json";
};

State s;

/* Map the .navmesh.json author path to the sibling binary that holds
 * the serialised Detour navmesh the runtime actually loads. */
static void derive_bin_path(const char *json_path, char *out, size_t out_sz)
{
    std::string p = (json_path && json_path[0]) ? json_path : "untitled.navmesh.json";
    const char *suffixes[] = { ".navmesh.json", ".json" };
    for (const char *sfx : suffixes) {
        size_t sl = std::strlen(sfx);
        if (p.size() >= sl && p.compare(p.size() - sl, sl, sfx) == 0) {
            p.erase(p.size() - sl);
            break;
        }
    }
    p += ".navmesh.bin";
    std::snprintf(out, out_sz, "%s", p.c_str());
}

/* Transform a model-space point by a column-major 4x4 (out = M*(x,y,z,1)). */
static inline void xform_point(const float *m, float x, float y, float z,
                               float out[3])
{
    out[0] = m[0] * x + m[4] * y + m[8]  * z + m[12];
    out[1] = m[1] * x + m[5] * y + m[9]  * z + m[13];
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

/* Triangle soup accumulator passed through jce_scene_each_entity. */
struct GatherCtx {
    JceScene             *scene = nullptr;
    std::vector<float>    verts;     /* xyz triplets, world space */
    std::vector<uint32_t> indices;
};

static void gather_entity(JceScene *scene, JceEntity e, void *ud)
{
    GatherCtx *g = (GatherCtx *)ud;
    if (!jce_scene_has_transform(scene, e)) return;

    jce_mat4 world = jce_scene_get_world_matrix(scene, e);
    const float *wm = JCE_M4_PTR(world);

    /* ── MeshRenderer: append its CPU triangles in world space. ── */
    if (jce_scene_has_mesh_renderer(scene, e)) {
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
        if (mr && mr->mesh_path[0]) {
            char host[1024];
            JceEditorCpuMeshData cpu;
            std::memset(&cpu, 0, sizeof(cpu));
            if (jce_editor_resolve_asset_path(mr->mesh_path, host, (int)sizeof(host)) &&
                jce_editor_model_load_cpu_file(host, &cpu) &&
                cpu.vertices && cpu.vertex_count >= 3 &&
                cpu.indices && cpu.index_count >= 3) {
                uint32_t base = (uint32_t)(g->verts.size() / 3);
                g->verts.reserve(g->verts.size() + (size_t)cpu.vertex_count * 3);
                for (uint32_t v = 0; v < cpu.vertex_count; ++v) {
                    const float *src = cpu.vertices[v].pos;
                    float w[3];
                    xform_point(wm, src[0], src[1], src[2], w);
                    g->verts.push_back(w[0]);
                    g->verts.push_back(w[1]);
                    g->verts.push_back(w[2]);
                }
                g->indices.reserve(g->indices.size() + cpu.index_count);
                for (uint32_t i = 0; i + 2 < cpu.index_count; i += 3) {
                    uint32_t i0 = cpu.indices[i + 0];
                    uint32_t i1 = cpu.indices[i + 1];
                    uint32_t i2 = cpu.indices[i + 2];
                    if (i0 >= cpu.vertex_count || i1 >= cpu.vertex_count ||
                        i2 >= cpu.vertex_count) continue;
                    g->indices.push_back(i0 + base);
                    g->indices.push_back(i1 + base);
                    g->indices.push_back(i2 + base);
                }
            }
            jce_editor_model_free_cpu_data(&cpu);
        }
    }

    /* ── Terrain: append a flat ground quad at the entity origin so the
     *    navmesh has a floor even without an authored ground mesh.  A
     *    full heightfield bake would sample the .terrain.json; this
     *    coarse quad keeps the chain functional. ── */
    if (jce_scene_has_terrain(scene, e)) {
        const float half = 64.0f; /* metres — generous default footprint */
        float quad[4][3] = {
            { -half, 0.0f, -half }, {  half, 0.0f, -half },
            {  half, 0.0f,  half }, { -half, 0.0f,  half },
        };
        uint32_t base = (uint32_t)(g->verts.size() / 3);
        for (auto &q : quad) {
            float w[3];
            xform_point(wm, q[0], q[1], q[2], w);
            g->verts.push_back(w[0]);
            g->verts.push_back(w[1]);
            g->verts.push_back(w[2]);
        }
        const uint32_t tri[6] = { 0, 1, 2, 0, 2, 3 };
        for (uint32_t t : tri) g->indices.push_back(base + t);
    }
}

/* ── Async Recast bake ────────────────────────────────────────────────
 * The Recast voxelisation/serialise (jce_recast_build_to_file) is 0.5-5s of
 * pure CPU and must NOT run on the UI thread. Split: gather triangle soup on
 * the main thread (reads the ECS scene — main-only), copy it into a job, run
 * the build on a worker, then finalise on the main thread. Recast is one
 * opaque call so "cancel" cannot interrupt it mid-build — it discards the
 * result on completion; the editor stays responsive throughout. */
struct NavBakeJob {
    std::vector<float>    verts;
    std::vector<uint32_t> indices;
    JceRecastConfig       rc{};
    char                  bin_path[300] = { 0 };
    uint32_t              vcount = 0, tcount = 0;
    JceRecastStats        stats = {};
    bool                  ok = false;
    JceAtomicI32         *done = nullptr;   /* 0 running, 1 finished */
};

static JceThread    *g_nav_worker = nullptr;
static NavBakeJob   *g_nav_job    = nullptr;
static JceAtomicI32 *g_nav_cancel = nullptr;

static void nav_bake_worker(void *arg)
{
    NavBakeJob *j = (NavBakeJob *)arg;
    j->ok = jce_recast_build_to_file(j->bin_path, j->verts.data(), j->vcount,
                                     j->indices.data(), j->tcount,
                                     &j->rc, &j->stats);
    jce_atomic_i32_store(j->done, 1);
}

/* MAIN thread: join the finished bake, apply or discard its result. */
static void nav_bake_finalize(void)
{
    NavBakeJob *j = g_nav_job;
    if (!j) return;
    if (g_nav_worker) { jce_thread_join(g_nav_worker); g_nav_worker = nullptr; }
    bool cancelled = g_nav_cancel && jce_atomic_i32_load(g_nav_cancel) != 0;

    if (cancelled) {
        jce_editor_console_log("navmesh bake cancelled (result discarded)");
    } else if (j->ok) {
        s.result.stats        = j->stats;
        s.result.in_vertices  = j->vcount;
        s.result.in_triangles = j->tcount;
        std::snprintf(s.result.out_path, sizeof(s.result.out_path), "%s",
                      j->bin_path);
        s.have_result = true;
        jce_editor_console_log(
            "navmesh baked: %u polys, %u verts, %d ms (from %u tris) -> %s",
            (unsigned)j->stats.polygon_count, (unsigned)j->stats.vertex_count,
            j->stats.build_time_ms, (unsigned)j->tcount, j->bin_path);
    } else {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "navmesh bake failed (Recast build/serialise error) -> %s",
            j->bin_path);
        s.have_result = false;
    }

    if (j->done) jce_atomic_i32_destroy(j->done);
    delete j;
    g_nav_job = nullptr;
}

/* MAIN thread, per-frame: pick up a finished bake. */
static void nav_bake_poll(void)
{
    if (g_nav_job && jce_atomic_i32_load(g_nav_job->done) != 0)
        nav_bake_finalize();
}

static bool nav_bake_running(void) { return g_nav_worker != nullptr; }

/* Gather scene triangles (main thread) and kick the Recast build onto a
 * worker. No-op if a bake is already in flight. */
void bake_recast(void)
{
    if (nav_bake_running()) return;

    const BakeSettings &c = s.cfg;
    JceScene *scene = jce_state_get_scene();
    if (!scene) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "navmesh bake: no active scene");
        return;
    }

    GatherCtx g;
    g.scene = scene;
    jce_scene_each_entity(scene, gather_entity, &g);   /* main: reads ECS */

    if (g.verts.size() < 9 || g.indices.size() < 3) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "navmesh bake: no walkable geometry found (need MeshRenderer "
            "or Terrain entities with loadable meshes)");
        return;
    }

    NavBakeJob *j = new NavBakeJob();
    j->verts   = std::move(g.verts);
    j->indices = std::move(g.indices);
    jce_recast_default_config(&j->rc);
    j->rc.cell_size          = c.cell_size;
    j->rc.cell_height        = c.cell_height;
    j->rc.walkable_slope_deg = c.max_slope_deg;
    j->rc.walkable_height    = c.agent_height;
    j->rc.walkable_climb     = c.climb_step;
    j->rc.walkable_radius    = c.agent_radius;
    derive_bin_path(s.path, j->bin_path, sizeof(j->bin_path));
    j->vcount = (uint32_t)(j->verts.size() / 3);
    j->tcount = (uint32_t)(j->indices.size() / 3);
    j->done   = jce_atomic_i32_create(0);

    if (!g_nav_cancel) g_nav_cancel = jce_atomic_i32_create(0);
    jce_atomic_i32_store(g_nav_cancel, 0);
    g_nav_job = j;

    g_nav_worker = jce_thread_create(nav_bake_worker, j, "jce_nav_bake");
    if (!g_nav_worker) {
        /* No worker thread available: run inline + finalise immediately. */
        nav_bake_worker(j);
        nav_bake_finalize();
    } else {
        jce_editor_console_log("navmesh bake started in background (%u tris)…",
                               (unsigned)j->tcount);
    }
}

JceJson *to_json(void)
{
    JceJson *root = jce_json_object();
    /* Settings. */
    JceJson *cfg = jce_json_object();
    jce_json_set_number(cfg, "cellSize",     s.cfg.cell_size);
    jce_json_set_number(cfg, "cellHeight",   s.cfg.cell_height);
    jce_json_set_number(cfg, "agentRadius",  s.cfg.agent_radius);
    jce_json_set_number(cfg, "agentHeight",  s.cfg.agent_height);
    jce_json_set_number(cfg, "maxSlopeDeg",  s.cfg.max_slope_deg);
    jce_json_set_number(cfg, "climbStep",    s.cfg.climb_step);
    jce_json_set_float_array(cfg, "boundsMin", s.cfg.bounds_min, 3);
    jce_json_set_float_array(cfg, "boundsMax", s.cfg.bounds_max, 3);
    jce_json_set_child(root, "settings", cfg);

    /* The baked navmesh lives in the .navmesh.bin sidecar; the .json now
     * carries only the (reproducible) bake settings. */
    return root;
}

void from_json(JceJson *root)
{
    JceJson *cfg = jce_json_get(root, "settings");
    if (cfg) {
        s.cfg.cell_size    = (float)jce_json_get_number(cfg, "cellSize",    s.cfg.cell_size);
        s.cfg.cell_height  = (float)jce_json_get_number(cfg, "cellHeight",  s.cfg.cell_height);
        s.cfg.agent_radius = (float)jce_json_get_number(cfg, "agentRadius", s.cfg.agent_radius);
        s.cfg.agent_height = (float)jce_json_get_number(cfg, "agentHeight", s.cfg.agent_height);
        s.cfg.max_slope_deg= (float)jce_json_get_number(cfg, "maxSlopeDeg", s.cfg.max_slope_deg);
        s.cfg.climb_step   = (float)jce_json_get_number(cfg, "climbStep",   s.cfg.climb_step);
        jce_json_get_floats(cfg, "boundsMin", s.cfg.bounds_min, 3, nullptr);
        jce_json_get_floats(cfg, "boundsMax", s.cfg.bounds_max, 3, nullptr);
    }
    s.have_result = false;
}

void save_to(const char *path)
{
    if (ed_write_json_to_file(path, to_json()))
        jce_editor_console_log("navmesh saved: %s", path);
    else
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "navmesh save failed: %s", path);
}

void load_from(const char *path)
{
    size_t sz = 0;
    char *buf = (char *)ed_read_file(path, &sz);
    if (!buf) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "navmesh load failed: %s", path);
        return;
    }
    JceJson *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!root) return;
    from_json(root);
    jce_json_free(root);
    /* Remember the last document that loaded OK (per-project). */
    jce_editor_pstate_set_str("doc.navmesh.last", path);
    jce_editor_console_log("navmesh loaded: %s", path);
}

void draw_settings(void)
{
    BakeSettings &c = s.cfg;
    ImGui::DragFloat (jce_editor_i18n_id("navmesh.field.cellSize",     "nav_cs"),  &c.cell_size,    0.01f, 0.05f, 5.0f, "%.2f m");
    ImGui::DragFloat (jce_editor_i18n_id("navmesh.field.cellHeight",   "nav_ch"),  &c.cell_height,  0.01f, 0.05f, 5.0f, "%.2f m");
    ImGui::DragFloat (jce_editor_i18n_id("navmesh.field.agentRadius",  "nav_ar"),  &c.agent_radius, 0.01f, 0.05f, 5.0f, "%.2f m");
    ImGui::DragFloat (jce_editor_i18n_id("navmesh.field.agentHeight",  "nav_ah"),  &c.agent_height, 0.01f, 0.05f, 8.0f, "%.2f m");
    ImGui::DragFloat (jce_editor_i18n_id("navmesh.field.maxSlope",     "nav_ms"),  &c.max_slope_deg,0.5f,  0.0f, 80.0f, "%.1f°");
    ImGui::DragFloat (jce_editor_i18n_id("navmesh.field.climbStep",    "nav_cst"), &c.climb_step,   0.01f, 0.0f, 5.0f, "%.2f m");
    /* Recast computes the bake AABB from the gathered geometry, so the
     * old bounds fields no longer gate the bake.  They are retained only
     * as informational hints for the (future) preview camera framing. */
    ImGui::DragFloat3(jce_editor_i18n_id("navmesh.field.boundsMin", "nav_bmin"), c.bounds_min, 0.5f);
    ImGui::DragFloat3(jce_editor_i18n_id("navmesh.field.boundsMax", "nav_bmax"), c.bounds_max, 0.5f);
}

void draw_actions(void)
{
    if (nav_bake_running()) {
        ImGui::TextUnformatted(jce_editor_i18n("navmesh.status.baking"));
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n_id("navmesh.button.cancel", "nav_cancel")) && g_nav_cancel)
            jce_atomic_i32_store(g_nav_cancel, 1);
    } else if (ImGui::Button(jce_editor_i18n_id("navmesh.button.bake", "nav_bake"))) {
        bake_recast();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("navmesh.button.clear", "nav_clr"))) {
        s.have_result = false;
        s.result = BakeResult();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("navmesh.button.save", "nav_save"))) save_to(s.path);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("navmesh.button.load", "nav_load"))) load_from(s.path);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(360);
    jce_draw_path_input(jce_editor_i18n_id("navmesh.field.path", "nav_path"), s.path, sizeof(s.path), JcePathKind::FileAbs);
    ImGui::SameLine();
    /* Proxy for the shared Scene-View show flag, so this checkbox and the
     * Scene View flags menu drive the SAME navmesh overlay switch. */
    bool overlay_on = jce_state_show_flag(JCE_SHOW_FLAG_NAVMESH);
    if (ImGui::Checkbox(jce_editor_i18n_id("navmesh.field.showOverlay", "nav_overlay"), &overlay_on))
        jce_state_set_show_flag(JCE_SHOW_FLAG_NAVMESH, overlay_on);
}

void draw_preview(void)
{
    if (!s.have_result) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(jce_theme::text_secondary()),
                           "%s", jce_editor_i18n("navmesh.label.noBake"));
        return;
    }
    const JceRecastStats &st = s.result.stats;
    ImGui::Text(jce_editor_i18n("navmesh.label.statPolys"),  st.polygon_count);
    ImGui::Text(jce_editor_i18n("navmesh.label.statVerts"),  st.vertex_count);
    ImGui::Text(jce_editor_i18n("navmesh.label.statTris"),   st.detail_triangle_count);
    ImGui::Text(jce_editor_i18n("navmesh.label.statTime"),   st.build_time_ms);
    ImGui::Text(jce_editor_i18n("navmesh.label.statInput"),
                (unsigned)s.result.in_triangles, (unsigned)s.result.in_vertices);
    if (s.result.out_path[0]) {
        ImGui::Separator();
        ImGui::TextWrapped("%s\n%s",
                           jce_editor_i18n("navmesh.label.statFile"),
                           s.result.out_path);
    }
}

void draw_content(void)
{
    /* One-time prefill of the last successfully loaded document
     * (per-project) so one click on Load reopens it.  Never auto-loads,
     * and never clobbers a path the user already typed. */
    static bool s_path_prefilled = false;
    if (!s_path_prefilled && jce_editor_pstate_active()) {
        s_path_prefilled = true;
        if (std::strcmp(s.path, "untitled.navmesh.json") == 0)
            jce_editor_pstate_get_str("doc.navmesh.last", s.path,
                                      sizeof(s.path));
    }

    nav_bake_poll();   /* pick up a finished background bake */
    if (ImGui::CollapsingHeader(jce_editor_i18n("navmesh.section.settings"), ImGuiTreeNodeFlags_DefaultOpen))
        draw_settings();
    ImGui::Separator();
    draw_actions();
    ImGui::Separator();
    if (ImGui::CollapsingHeader(jce_editor_i18n("navmesh.section.preview"), ImGuiTreeNodeFlags_DefaultOpen))
        draw_preview();
}

} /* namespace */

extern "C" void jce_editor_panel_navmesh(void)
{
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###jce_navmesh", jce_editor_i18n("navmesh.title"));
    if (!ImGui::Begin(_wt,
                      jce_editor_panel_visible_ptr(JCE_PANEL_NAVMESH), ImGuiWindowFlags_NoFocusOnAppearing))
    {
        ImGui::End();
        return;
    }
    draw_content();
    ImGui::End();
}
