/*
 * jce_editor_state.cpp  Central editor state — core definitions.
 *
 * Defines the global editor state struct, selection, entity CRUD,
 * component management, mode accessors, and demo scene builder.
 *
 * After the ECS-direct refactor, the engine's JceScene (flecs) is the
 * single source of truth for all entity / component data.  The editor
 * keeps only UI-specific state (selection, modes, undo history, sidecar
 * fold flags) and an order vector for stable iteration / sibling ordering.
 *
 * Parsing, serialization, history, play mode, and prefabs live in their
 * own jce_editor_*.cpp files.
 */

#include "jce_editor_config.h"
#include "jce_editor_component_registry.h"
#include "jce_editor_i18n.h"
#include "jce_editor_scene_rendering_defaults.h"
#include "jce_editor_state_internal.h"
#include "core/jce_editor_project_state.h"   /* per-project view mode / grid */
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"

#include <jce/os/core/jce_perf_phase.h>   /* benchmark: capture CPU phases */
#include <jce/renderer/jce_lowlevel.h>    /* stress: runtime albedo textures */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>
#include <string>

#include <jce/os/core/jce_str.h>
#include <jce/middleware/physics/jce_cloth.h>
#include <jce/resource/jce_world_streamer.h>

/* Forward-declare only the functions we need from scene_render,
   avoiding a full include that creates a cpp-level circular dependency. */
extern "C" void jce_editor_scene_set_scene_dir(const char *dir);
extern "C" void jce_editor_scene_render_streaming_teardown(void);
extern "C" void jce_editor_scene_render_streaming_rebuild(void);
extern "C" struct JceWorldStreamer *jce_editor_get_world_streamer(void);

/* ── Global State Definitions ─────────────────────────────────────── */

EditorInternalState s;

std::vector<uint32_t>                              g_entity_order;
uint64_t                                           g_entity_order_gen;
std::unordered_map<uint32_t, EditorEntitySidecar>  g_entity_sidecar;

std::vector<EditorHistorySnapshot> s_undo_history;
std::vector<EditorHistorySnapshot> s_redo_history;
int  s_history_suspend_depth = 0;
int  s_history_edit_nesting = 0;
bool s_history_outer_edit_pushed_snapshot = false;
int  s_history_manual_batch_depth = 0;
int  s_history_transient_batch_depth = 0;

EditorTransaction s_transaction;

/* Gizmo Ctrl-snap increments — cached in memory, loaded from config on
 * init, persisted on change.  Defaults match the historical hardcoded
 * values (0.5 units / 15 deg / 0.25 ratio). */
static float s_gizmo_snap_translate = 0.5f;
static float s_gizmo_snap_rotate    = 15.0f;
static float s_gizmo_snap_scale     = 0.25f;
/* Persistent snap toggle: when on, gizmo drags snap WITHOUT holding Ctrl
 * (Ctrl still forces snap momentarily). Persists per user. */
static bool  s_gizmo_snap_enabled   = false;

/* ── Internal helpers ─────────────────────────────────────────────── */

static void erase_from_order(uint32_t id)
{
    auto it = std::find(g_entity_order.begin(), g_entity_order.end(), id);
    if (it != g_entity_order.end())
        g_entity_order.erase(it); g_entity_order_gen++;
}

static void rebuild_order_cb(JceScene * /*scene*/, JceEntity e, void *user_data)
{
    auto *out = static_cast<std::vector<uint32_t> *>(user_data);
    out->push_back((uint32_t)e);
}

void rebuild_entity_order_from_ecs(void)
{
    g_entity_order.clear(); g_entity_order_gen++;
    if (!s.scene) return;
    jce_scene_each_entity(s.scene, rebuild_order_cb, &g_entity_order);
}

void set_current_scene_path_internal(const char *scene_path)
{
    if (scene_path && scene_path[0] != '\0')
        snprintf(s.current_scene_path, sizeof(s.current_scene_path), "%s", scene_path);
    else
        s.current_scene_path[0] = '\0';
}

void update_scene_dir_from_path(const char *scene_path)
{
    if (!scene_path || scene_path[0] == '\0')
        return;

    char scene_dir[512];
    snprintf(scene_dir, sizeof(scene_dir), "%s", scene_path);

    /* Trim to parent directory. */
    jce_editor_path_trim_to_parent(scene_dir);
    if (scene_dir[0]) {
        /* Go up one more level if we're in a "Scenes" subdirectory. */
        const char *dir_name = jce_editor_path_basename_view(scene_dir);
        if (jce_strcasecmp(dir_name, "Scenes") == 0) {
            jce_editor_path_trim_to_parent(scene_dir);
        }
    }

    jce_editor_scene_set_scene_dir(scene_dir);
}

void clear_scene_entities(void)
{
    g_entity_order.clear(); g_entity_order_gen++;
    g_entity_sidecar.clear();

    /* Destroy and recreate engine scene to clear all ECS entities. */
    if (s.scene) {
        jce_scene_destroy(s.scene);
        s.scene = jce_scene_create();
    }
    jce_state_clear_selection();
}

/* When true, jce_state_add_component() omits its per-call INFO log.
 * Used by the stress-test path to avoid flooding the console with
 * thousands of identical lines. */
static bool s_suppress_add_component_log = false;

/* ── Demo scene builder ──────────────────────────────────────────── */

static void demo_set_position(uint32_t id, float x, float y, float z)
{
    JceTransform *t = jce_scene_get_transform(s.scene, (JceEntity)id);
    if (!t) return;
    t->position = jce_v3(x, y, z);
}

static void demo_set_scale(uint32_t id, float x, float y, float z)
{
    JceTransform *t = jce_scene_get_transform(s.scene, (JceEntity)id);
    if (!t) return;
    t->scale = jce_v3(x, y, z);
}

static void demo_set_mesh_shape(uint32_t id, int shape)
{
    JceMeshRenderer *m = jce_scene_get_mesh_renderer(s.scene, (JceEntity)id);
    if (!m) return;
    m->mesh_shape = shape;
}

static void build_demo_scene(void)
{
    HistorySuspendScope no_undo;

    uint32_t root = jce_state_create_entity("Scene Root", 0);

    uint32_t cam = jce_state_create_entity("Main Camera", root);
    demo_set_position(cam, 0.0f, 5.0f, 10.0f);
    jce_state_add_component(cam, JCE_COMP_FLAG_CAMERA);

    uint32_t lights = jce_state_create_entity("Lights", root);
    uint32_t dir_light = jce_state_create_entity("Directional Light", lights);
    jce_state_add_component(dir_light, JCE_COMP_FLAG_DIR_LIGHT);

    uint32_t pt_light = jce_state_create_entity("Point Light", lights);
    jce_state_add_component(pt_light, JCE_COMP_FLAG_POINT_LIGHT);

    uint32_t objs = jce_state_create_entity("Objects", root);

    uint32_t cube = jce_state_create_entity("Cube", objs);
    jce_state_add_component(cube, JCE_COMP_FLAG_MESH_RENDERER);
    demo_set_position(cube, 0.0f, 0.5f, 0.0f);

    uint32_t sphere = jce_state_create_entity("Sphere", objs);
    jce_state_add_component(sphere, JCE_COMP_FLAG_MESH_RENDERER);
    demo_set_position(sphere, 2.5f, 0.5f, 0.0f);
    demo_set_mesh_shape(sphere, 1 /* sphere */);

    uint32_t plane = jce_state_create_entity("Plane", objs);
    jce_state_add_component(plane, JCE_COMP_FLAG_MESH_RENDERER);
    demo_set_position(plane, -2.5f, 0.0f, 0.0f);
    demo_set_scale(plane, 3.0f, 3.0f, 3.0f);
    demo_set_mesh_shape(plane, 2 /* plane */);

    uint32_t capsule = jce_state_create_entity("Capsule (no texture)", objs);
    jce_state_add_component(capsule, JCE_COMP_FLAG_MESH_RENDERER);
    demo_set_position(capsule, -2.5f, 1.0f, 2.0f);
    demo_set_mesh_shape(capsule, 3 /* capsule */);

    uint32_t cylinder = jce_state_create_entity("Cylinder (textured)", objs);
    jce_state_add_component(cylinder, JCE_COMP_FLAG_MESH_RENDERER);
    demo_set_position(cylinder, 2.5f, 0.5f, 2.0f);
    demo_set_mesh_shape(cylinder, 4 /* cylinder */);
    if (JceMeshRenderer *m = jce_scene_get_mesh_renderer(s.scene, (JceEntity)cylinder)) {
        m->base_color[0] = 0.2f;
        m->base_color[1] = 0.6f;
        m->base_color[2] = 0.9f;
        m->base_color[3] = 1.0f;
        m->metallic  = 0.3f;
        m->roughness = 0.5f;
    }

    uint32_t ui = jce_state_create_entity("UI", root);
    jce_state_create_entity("Canvas", ui);

    /* ── Optional stress test (env JCE_STRESS_CUBES=N) ─────────────
     * Spawns N cubes on a uniform 3D grid centred at origin so we can
     * benchmark frustum culling, grid persistence, and LOD selection
     * with a non-trivial entity count. */
    {
        const char *env = std::getenv("JCE_STRESS_CUBES");
        if (env) {
            long n = strtol(env, nullptr, 10);
            /* The 200k guard protects the per-ENTITY grid path (200k ECS
             * entities); the SCATTER path is ONE entity with N instances and
             * takes authored counts into the tens of millions (千万 S5 tiled
             * streaming) — cap it at 100M instead. */
            const long n_cap = std::getenv("JCE_STRESS_SCATTER") ? 100000000L
                                                                 : 200000L;
            if (n > n_cap) n = n_cap;
            if (n > 0) {
              /* JCE_STRESS_SCATTER + JCE_STRESS_MODEL_PATH: the Unity-ISM / UE-HISM
               * path — ONE VegetationScatter entity holding N instances, drawn as a
               * single GPU-instanced submit (sr_draw_foliage), instead of N ECS
               * entities.  This is how a large scene's repeated objects reach
               * near-zero per-instance CPU. */
              const char *scatter_model = std::getenv("JCE_STRESS_MODEL_PATH");
              if (std::getenv("JCE_STRESS_SCATTER")) {
                /* JCE_STRESS_OCCLUDER: put the scatter field BEHIND a big opaque
                 * wall (a MeshRenderer box → in the depth prepass) so the foliage
                 * Hi-Z occlusion cull has a real occluder to test against — the
                 * canonical heavy-occlusion repro (the default flat field has no
                 * occluder, so only frustum cull ever engages).  The default
                 * grazing vista camera (~eye z=-40 looking +z) sees the wall with
                 * the field hidden behind it. */
                const bool occ = (std::getenv("JCE_STRESS_OCCLUDER") != nullptr);
                uint32_t sc = jce_state_create_entity("Instanced Scatter", root);
                JceVegetationScatterComponent vs;
                memset(&vs, 0, sizeof vs);
                vs.visible = true;
                vs.mesh_shape = 0;  /* cube primitive when no model path given */
                if (scatter_model && scatter_model[0])
                    snprintf(vs.mesh_path, sizeof vs.mesh_path, "%s", scatter_model);
                float side_m = (float)sqrt((double)n) * 2.0f;  /* spread so cubes don't fully overlap */
                if (occ) {
                    /* Bounded corridor behind the wall (x∈[-40,40], z∈[10,~210]). */
                    vs.area_x = 80.0f; vs.area_z = 200.0f;
                    vs.density = (float)n / (vs.area_x * vs.area_z);
                    demo_set_position(sc, 0.0f, 0.0f, 110.0f);   /* field centre z=110 → z∈[10,210] */
                } else {
                    vs.area_x = side_m; vs.area_z = side_m;
                    vs.density = (float)n / (side_m * side_m);
                    demo_set_position(sc, 0.0f, 0.0f, 0.0f);
                }
                vs.scale_min = 1.0f; vs.scale_max = 1.0f;
                vs.max_slope_deg = 90.0f; vs.seed = 1u;
                /* JCE_STRESS_SHADOW: the scatter field casts shadows (千万 ③ —
                 * one instanced depth draw per cascade at reduced LOD). */
                vs.cast_shadow = (std::getenv("JCE_STRESS_SHADOW") != nullptr);
                jce_scene_set_vegetation_scatter(s.scene, (JceEntity)sc, &vs);
                if (occ) {
                    /* Opaque wall at z≈4 spanning the corridor's front, tall enough
                     * to hide the field but leaving a strip of sky/edges so some
                     * foliage stays visible (parity anchor). */
                    uint32_t w = jce_state_create_entity("OccluderWall", root);
                    jce_state_add_component(w, JCE_COMP_FLAG_MESH_RENDERER);
                    demo_set_position(w, 0.0f, 18.0f, 4.0f);
                    demo_set_scale(w, 90.0f, 36.0f, 2.0f);       /* x∈[-45,45] y∈[0,36] */
                    LOG_INFO(LOG_TAG, "stress OCCLUDER: opaque wall in front of the "
                             "scatter field (foliage Hi-Z occlusion A/B)");
                }
                LOG_INFO(LOG_TAG, "stress SCATTER: ~%ld instances of '%s' "
                         "(1 entity, GPU-instanced)%s", n,
                         (scatter_model && scatter_model[0]) ? scatter_model : "primitive cube",
                         occ ? " [behind occluder wall]" : "");
              } else {
                if (const char *oc = std::getenv("JCE_STRESS_OCCLUDER")) {
                    /* Same opaque wall as the scatter branch, for the MODEL
                     * path (Nanite-lite meshlet Hi-Z A/B): the stress grid
                     * sits at the origin behind it as seen from the vista
                     * eye (~z=-40 looking +z); z=-30 clears a 20m-radius
                     * hero mesh while hiding it.  A numeric value sets the
                     * wall HEIGHT (metres): a half wall leaves the hero's
                     * top visible, so the ENTITY-level Hi-Z keeps it and
                     * only the CLUSTER-level cull can drop the hidden
                     * clusters — the partial-occlusion A/B this feature is
                     * for ("1" = legacy full 36m wall). */
                    float wall_h = (float)atof(oc);
                    if (wall_h <= 1.0f) wall_h = 36.0f;
                    uint32_t w = jce_state_create_entity("OccluderWall", root);
                    jce_state_add_component(w, JCE_COMP_FLAG_MESH_RENDERER);
                    demo_set_position(w, 0.0f, wall_h * 0.5f, -30.0f);
                    demo_set_scale(w, 90.0f, wall_h, 2.0f);
                    LOG_INFO(LOG_TAG, "stress OCCLUDER: opaque wall (h=%.0fm) in "
                             "front of the stress grid (meshlet Hi-Z A/B)", wall_h);
                }
                LOG_INFO(LOG_TAG, "stress test: spawning %ld cubes", n);
                uint32_t stress_root = jce_state_create_entity("Stress Cubes", root);

                /* Cube root of N rounded up so the side length covers all. */
                int side = 1;
                while ((long)side * side * side < n) side++;
                /* JCE_STRESS_SPACING overrides the grid spacing so the field can
                 * spread beyond the SimLod mid-radius (80m) for the #5 sim-LOD
                 * physics-gating demo (far bodies sleep). */
                float spacing = 1.5f;
                if (const char *sp = std::getenv("JCE_STRESS_SPACING")) {
                    float v = (float)atof(sp); if (v > 0.01f) spacing = v;
                }
                /* JCE_STRESS_PHYS_SIMLOD: add a SimLod component (gate PHYSICS) so
                 * the runtime sleeps far-tier (>mid_radius from the viewer) rigid
                 * bodies — demonstrates sim-LOD cutting large-world physics cost. */
                const bool phys_simlod = (std::getenv("JCE_STRESS_PHYS_SIMLOD") != NULL);
                const float origin  = -0.5f * (side - 1) * spacing;

                /* JCE_STRESS_MODEL_PATH=<glTF abs path> makes each stress entity a
                 * real model instance instead of a primitive cube — lets us profile
                 * the MODEL render path (the common real-game content) at scale. */
                const char *stress_model = std::getenv("JCE_STRESS_MODEL_PATH");
                /* JCE_STRESS_DIVERSE gives each cube a unique base colour =>
                 * unique material key => the auto-instancer can't batch them =>
                 * one draw per cube.  This is the CPU-bound HIGH-DRAW-COUNT
                 * workload that multi-threaded command submission targets (the
                 * default uniform stress collapses to a single instanced batch). */
                const bool diverse = (std::getenv("JCE_STRESS_DIVERSE") != NULL);
                /* JCE_STRESS_PHYSICS (#5 physics benchmark): give each stress cube a
                 * DYNAMIC rigid body + box collider so Play (JCE_KPI_AUTOPLAY) free-
                 * falls/collides N bodies — the standard Bullet broadphase + solver
                 * stress.  Measured via the runtime "physics" perf-phase. */
                const bool phys = (std::getenv("JCE_STRESS_PHYSICS") != NULL);
                /* JCE_STRESS_PHYS_MOVERS=N: give ONLY the first N cubes a dynamic
                 * rigidbody (the rest stay static) — a MIXED static+dynamic-caster
                 * scene for the CSM static/dynamic shadow-separation work.  Unlike
                 * JCE_STRESS_MOVERS (set_transform, which bumps the caster key and
                 * is classified static), rigidbody movers mutate in place → the
                 * shadow system's is_dyn_caster path keeps the static caster key
                 * stable, so a static-shadow cache can actually survive their
                 * motion.  Needs JCE_KPI_AUTOPLAY to start the simulation. */
                const long phys_movers = std::getenv("JCE_STRESS_PHYS_MOVERS")
                                       ? atol(std::getenv("JCE_STRESS_PHYS_MOVERS")) : 0;
                s_suppress_add_component_log = true;

                /* JCE_STRESS_TEXTURES=D: create D distinct 4x4 RGBA8 runtime
                 * albedo textures and assign them round-robin as per-entity
                 * runtime albedos — the texture-diverse instancing workload
                 * (same mesh, distinct albedo TEXTURE each → distinct material
                 * key → solo, unless JCE_TEX_INSTANCE packs them into a 2D-array).
                 * Capped at 64 (the array's layer limit). */
                int  n_tex = 0;
                uint16_t tex_ids[256];
                if (const char *te = std::getenv("JCE_STRESS_TEXTURES")) {
                    n_tex = atoi(te);
                    if (n_tex < 0) n_tex = 0;
                    if (n_tex > 256) n_tex = 256;   /* >64 exercises the multi-array split */
                    const int TS = 32;   /* 32x32 with a full mip chain so distant
                                          * instances sample real mips — verifies the
                                          * array's per-mip blit (high-freq checker so
                                          * a no-mip array would visibly alias). */
                    for (int t = 0; t < n_tex; t++) {
                        uint8_t r = (uint8_t)(30 + (t * 53)  % 220);
                        uint8_t g = (uint8_t)(30 + (t * 97)  % 220);
                        uint8_t b = (uint8_t)(30 + (t * 151) % 220);
                        /* Build the concatenated mip pyramid (bgfx expects all
                         * levels when has_mips=true). L0 = distinct-colour checker
                         * vs white; each level box-downsamples the previous. */
                        std::vector<uint8_t> prev, cur;
                        std::vector<uint8_t> buf;
                        int dim = TS;
                        prev.resize((size_t)dim * dim * 4);
                        for (int y = 0; y < dim; y++)
                            for (int x = 0; x < dim; x++) {
                                bool white = (((x >> 2) + (y >> 2)) & 1) != 0;
                                uint8_t *p = &prev[((size_t)y * dim + x) * 4];
                                p[0] = white ? 255 : r; p[1] = white ? 255 : g;
                                p[2] = white ? 255 : b; p[3] = 255;
                            }
                        buf.insert(buf.end(), prev.begin(), prev.end());
                        while (dim > 1) {
                            int nd = dim >> 1; if (nd < 1) nd = 1;
                            cur.assign((size_t)nd * nd * 4, 0);
                            for (int y = 0; y < nd; y++)
                                for (int x = 0; x < nd; x++)
                                    for (int c = 0; c < 4; c++) {
                                        int s = 0;
                                        s += prev[(((size_t)(2*y)  * dim + (2*x)  ) * 4) + c];
                                        s += prev[(((size_t)(2*y)  * dim + (2*x+1)) * 4) + c];
                                        s += prev[(((size_t)(2*y+1)* dim + (2*x)  ) * 4) + c];
                                        s += prev[(((size_t)(2*y+1)* dim + (2*x+1)) * 4) + c];
                                        cur[((size_t)y * nd + x) * 4 + c] = (uint8_t)(s / 4);
                                    }
                            buf.insert(buf.end(), cur.begin(), cur.end());
                            prev.swap(cur);
                            dim = nd;
                        }
                        const JceGfxMemory *mem = jce_gfx_memory_copy(buf.data(), (uint32_t)buf.size());
                        JceTextureHandle h = jce_texture_create_2d(
                            (uint16_t)TS, (uint16_t)TS, true, 1, JCE_TEXTURE_FORMAT_RGBA8, 0, mem);
                        tex_ids[t] = (uint16_t)h.idx;
                    }
                    LOG_INFO(LOG_TAG, "stress TEXTURES: created %d distinct runtime albedos", n_tex);
                }

                /* JCE_STRESS_TEX_PATHS=p1,p2,...: point each cube's albedo at a
                 * real texture FILE (round-robin) — exercises the batcher's
                 * PATH-albedo branch (sr_resolve_texture + jce_texture_get_size /
                 * _mips on registry-loaded textures) with genuine cooked content,
                 * vs the runtime-handle path JCE_STRESS_TEXTURES uses. */
                std::vector<std::string> tex_paths;
                if (const char *tp = std::getenv("JCE_STRESS_TEX_PATHS")) {
                    const char *s = tp;
                    while (*s) {
                        const char *e = std::strchr(s, ',');
                        size_t len = e ? (size_t)(e - s) : std::strlen(s);
                        if (len > 0) tex_paths.emplace_back(s, len);
                        if (!e) break;
                        s = e + 1;
                    }
                    LOG_INFO(LOG_TAG, "stress TEX_PATHS: %zu real albedo files", tex_paths.size());
                }

                long spawned = 0;
                for (int x = 0; x < side && spawned < n; x++) {
                    for (int y = 0; y < side && spawned < n; y++) {
                        for (int z = 0; z < side && spawned < n; z++) {
                            uint32_t c = jce_state_create_entity("c", stress_root);
                            jce_state_stress_record_mover(c);
                            jce_state_add_component(c, JCE_COMP_FLAG_MESH_RENDERER);
                            JceMeshRenderer *mm =
                                jce_scene_get_mesh_renderer(s.scene, (JceEntity)c);
                            if (mm && stress_model && stress_model[0])
                                snprintf(mm->mesh_path, sizeof mm->mesh_path,
                                         "%s", stress_model);
                            if (mm && diverse) {
                                mm->base_color[0] = 0.15f + 0.7f * (float)((spawned * 13) % 101) / 101.0f;
                                mm->base_color[1] = 0.15f + 0.7f * (float)((spawned * 37) % 103) / 103.0f;
                                mm->base_color[2] = 0.15f + 0.7f * (float)((spawned * 71) % 107) / 107.0f;
                                mm->base_color[3] = 1.0f;   /* authored marker */
                            }
                            if (mm && n_tex > 0) {
                                mm->has_albedo_runtime = true;
                                mm->albedo_runtime_idx = tex_ids[(int)(spawned % n_tex)];
                                mm->albedo_runtime_w = 32;
                                mm->albedo_runtime_h = 32;
                                mm->albedo_runtime_mips = 6;   /* 32x32 full chain */
                            }
                            if (mm && !tex_paths.empty()) {
                                const std::string &p = tex_paths[(size_t)(spawned % (long)tex_paths.size())];
                                snprintf(mm->albedo_tex, sizeof mm->albedo_tex, "%s", p.c_str());
                            }
                            if (phys || spawned < phys_movers) {
                                jce_state_add_component(c, JCE_COMP_FLAG_RIGIDBODY);
                                jce_state_add_component(c, JCE_COMP_FLAG_BOX_COLLIDER);
                                if (JceRigidBodyComponent *rb =
                                        jce_scene_get_rigidbody(s.scene, (JceEntity)c)) {
                                    rb->body_type = 1;   /* JCE_BODY_DYNAMIC */
                                    rb->mass      = 1.0f;
                                }
                                if (JceBoxColliderComponent *bc =
                                        jce_scene_get_box_collider(s.scene, (JceEntity)c)) {
                                    bc->size[0] = bc->size[1] = bc->size[2] = 1.0f;
                                }
                                if (phys_simlod) {
                                    JceSimLodComponent sl;
                                    memset(&sl, 0, sizeof sl);
                                    sl.enabled     = true;
                                    sl.near_radius = 25.0f;
                                    sl.mid_radius  = 80.0f;
                                    sl.far_hz      = -1.0f;   /* pause far tier */
                                    sl.gate_mask   = JCE_SIMLOD_GATE_PHYSICS;
                                    jce_scene_set_sim_lod(s.scene, (JceEntity)c, &sl);
                                }
                            }
                            demo_set_position(c,
                                              origin + x * spacing,
                                              origin + y * spacing,
                                              origin + z * spacing);
                            spawned++;
                        }
                    }
                }
                s_suppress_add_component_log = false;
                LOG_INFO(LOG_TAG, "stress test: spawned %ld %s (%dx%dx%d grid)",
                         spawned, (stress_model && stress_model[0]) ? "models" : "cubes",
                         side, side, side);
              } /* end else (N-entity path) */
            }
        }
    }

    /* ── Optional many-light stress (JCE_STRESS_LIGHTS=N) ──────────────
     * Spawns N point lights scattered over the stress field so the many-light
     * path is measurable: Forward+ (r.forwardplus / JCE_FORWARDPLUS) clusters
     * them via the s_cluster data texture, vs the brute-force per-draw
     * u_pointLights uniform arrays — the SRP-Batcher-equivalent A/B for lights. */
    {
        const char *lenv = std::getenv("JCE_STRESS_LIGHTS");
        if (lenv) {
            long nl = strtol(lenv, nullptr, 10);
            if (nl > 0 && nl <= 4096) {
                long ncubes = 0;
                if (const char *ce = std::getenv("JCE_STRESS_CUBES"))
                    ncubes = strtol(ce, nullptr, 10);
                float span = (float)sqrt((double)(ncubes > 0 ? ncubes : 4096)) * 1.5f;
                uint32_t lroot = jce_state_create_entity("Stress Lights", root);
                s_suppress_add_component_log = true;
                for (long li = 0; li < nl; li++) {
                    uint32_t le = jce_state_create_entity("pl", lroot);
                    jce_state_add_component(le, JCE_COMP_FLAG_POINT_LIGHT);
                    float fx = ((float)((li * 131) % 997) / 997.0f - 0.5f) * span;
                    float fz = ((float)((li * 271) % 991) / 991.0f - 0.5f) * span;
                    JcePointLight pl; memset(&pl, 0, sizeof pl);
                    pl.position.x = fx; pl.position.y = 3.0f; pl.position.z = fz;
                    pl.color.x = 1.0f; pl.color.y = 0.85f; pl.color.z = 0.7f;
                    pl.intensity = 4.0f; pl.radius = 10.0f;
                    jce_scene_set_point_light(s.scene, (JceEntity)le, &pl);
                    demo_set_position(le, fx, 3.0f, fz);
                }
                s_suppress_add_component_log = false;
                LOG_INFO(LOG_TAG, "stress LIGHTS: spawned %ld point lights", nl);
            }
        }
    }

    /* ── Optional skinned-crowd stress (rank 5b) ──────────────────────
     * JCE_STRESS_SKINNED=N + JCE_STRESS_SKIN_PATH=<animated glb abs path>
     * spawns N skinned characters (MeshRenderer + SkeletalAnimator playing a
     * looping clip) on a 2D grid, so the skinning CPU path (pose sample +
     * skeleton eval + world-palette build) is measurable — the cube harness
     * only exercises static meshes.  Pairs with the rank-9 alloc counter to
     * expose per-frame heap churn in jce_skeleton_evaluate.  Optional:
     * JCE_STRESS_SKIN_CLIP (default "Run"), JCE_STRESS_SKIN_SCALE (default 0.02
     * so the model isn't huge/GPU-bound). */
    {
        const char *skenv  = std::getenv("JCE_STRESS_SKINNED");
        const char *skpath = std::getenv("JCE_STRESS_SKIN_PATH");
        if (skenv) {
            const long n = strtol(skenv, nullptr, 10);
            if (n > 0 && n <= 50000 && skpath && skpath[0]) {
                LOG_INFO(LOG_TAG, "stress SKINNED: spawning %ld animated '%s'", n, skpath);
                uint32_t sroot = jce_state_create_entity("Stress Skinned", root);
                int side = 1; while ((long)side * side < n) side++;
                const float spacing = 2.0f;
                const float org = -0.5f * (float)(side - 1) * spacing;
                const char *clip = std::getenv("JCE_STRESS_SKIN_CLIP");
                float scl = 0.02f;
                if (const char *se = std::getenv("JCE_STRESS_SKIN_SCALE")) {
                    float v = (float)atof(se); if (v > 0.0f) scl = v;
                }
                s_suppress_add_component_log = true;
                long spawned = 0;
                for (int x = 0; x < side && spawned < n; x++)
                for (int z = 0; z < side && spawned < n; z++) {
                    uint32_t c = jce_state_create_entity("sk", sroot);
                    jce_state_add_component(c, JCE_COMP_FLAG_MESH_RENDERER);
                    if (JceMeshRenderer *mm = jce_scene_get_mesh_renderer(s.scene, (JceEntity)c))
                        snprintf(mm->mesh_path, sizeof mm->mesh_path, "%s", skpath);
                    jce_state_add_component(c, JCE_COMP_FLAG_SKELETAL_ANIMATOR);
                    if (JceSkeletalAnimatorComponent *sa =
                            jce_scene_get_skeletal_animator(s.scene, (JceEntity)c)) {
                        snprintf(sa->skeleton_path, sizeof sa->skeleton_path, "%s", skpath);
                        snprintf(sa->clip_names[0], sizeof sa->clip_names[0], "%s",
                                 (clip && clip[0]) ? clip : "Run");
                        sa->clip_count = 1;
                        sa->active_clip = 0;
                        sa->speed = 1.0f; sa->loop = true; sa->playing = true;
                    }
                    demo_set_position(c, org + (float)x * spacing, 0.0f, org + (float)z * spacing);
                    demo_set_scale(c, scl, scl, scl);
                    spawned++;
                }
                s_suppress_add_component_log = false;
                LOG_INFO(LOG_TAG, "stress SKINNED: spawned %ld (%dx%d grid, clip '%s')",
                         spawned, side, side, (clip && clip[0]) ? clip : "Run");
            } else if (n > 0) {
                LOG_WARN(LOG_TAG, "JCE_STRESS_SKINNED needs JCE_STRESS_SKIN_PATH=<animated glb absolute path>");
            }
        }
    }
}

/* ── Init / Shutdown ─────────────────────────────────────────────── */

/* Defined with the view/gizmo toggles near the bottom of this file. */
static void load_persisted_view_toggles(void);

void jce_editor_state_init(bool with_demo_scene)
{
    memset(&s, 0, sizeof(s));
    g_entity_order.clear(); g_entity_order_gen++;
    g_entity_sidecar.clear();
    s_undo_history.clear();
    s_redo_history.clear();
    s_history_suspend_depth = 0;
    s_history_edit_nesting = 0;
    s_history_outer_edit_pushed_snapshot = false;
    s_history_manual_batch_depth = 0;
    s_history_transient_batch_depth = 0;
    s_transaction.active = false;
    s_transaction.label[0] = '\0';
    s_transaction.before.scene_json.clear();
    s_transaction.before.scene_path.clear();

    s.edit_mode   = JCE_EDIT_MODE_SELECT;
    s.gizmo_mode  = JCE_GIZMO_TRANSLATE;
    s.gizmo_space = JCE_GIZMO_LOCAL;
    s.gizmo_pivot = JCE_GIZMO_PIVOT;
    s.play_state  = JCE_PLAY_STOPPED;
    s.current_scene_path[0] = '\0';

    /* Load persisted render settings from the split per-user config
     * (~/.jce/editor-session.json: view_mode/show_grid;
     *  ~/.jce/editor-preferences.json: gizmo snap increments). */
    {
        JceEditorConfig ecfg;
        if (jce_editor_config_load(&ecfg)) {
            int vm = ecfg.view_mode;
            if (vm >= JCE_VIEW_SHADED && vm <= JCE_VIEW_AO)
                s.view_mode = (JceSceneViewMode)vm;
            else
                s.view_mode = JCE_VIEW_SHADED;
            s.show_grid = ecfg.show_grid;
            s_gizmo_snap_translate = ecfg.gizmo_snap_translate;
            s_gizmo_snap_rotate    = ecfg.gizmo_snap_rotate;
            s_gizmo_snap_scale     = ecfg.gizmo_snap_scale;
        } else {
            s.view_mode = JCE_VIEW_SHADED;
            s.show_grid = true;
        }
    }

    /* Restore the user-global view/gizmo toggles (show flags, gizmo
     * mode/space/pivot, snap toggle, 2D mode) saved by their setters. */
    load_persisted_view_toggles();

    s.scene = jce_scene_create();
    if (!s.scene) {
        LOG_ERROR(LOG_TAG, "failed to create engine scene");
    }

    if (with_demo_scene) {
        /* Note: clear_scene_entities() is intentionally NOT called here.
           It would destroy the scene we just created and recreate it,
           plus clear containers/selection that are already empty after
           the memset() above — pure churn (~visible as a stray
           "scene destroyed / scene created" pair in startup logs). */
        HistorySuspendScope suspend;
        /* Suppress per-component "add component …" spam during demo
         * scene construction.  The single "editor state initialized
         * (N demo entities)" line below is sufficient. */
        s_suppress_add_component_log = true;
        build_demo_scene();
        s_suppress_add_component_log = false;
    }
    /* Always ensure the scene has render settings (empty or demo). */
    jce_editor_scene_ensure_rendering_settings(s.scene);

    s.initialized = true;
    LOG_INFO(LOG_TAG, "editor state initialized (%d entities%s)",
             (int)g_entity_order.size(),
             with_demo_scene ? ", demo" : ", empty — restoring scene");
}

bool jce_state_new_default_scene(void)
{
    /* If Play is running it holds the current JceScene; recreating /
     * clearing it here would be a use-after-free.  Tear the runtime down
     * first. */
    stop_play_before_scene_swap();

    /* The streaming-preview streamer holds entity handles into the old
     * scene — destroy it BEFORE the entities go away. */
    jce_editor_scene_render_streaming_teardown();

    /* New Scene is not a file load, so explicitly leave read-only Bundle
     * Preview before constructing project-backed default content. */
    jce_state_close_bundle_preview();

    if (!s.scene)
        s.scene = jce_scene_create();
    else
        clear_scene_entities();

    if (!s.scene) {
        LOG_ERROR(LOG_TAG, "new default scene failed: cannot create engine scene");
        return false;
    }

    {
        HistorySuspendScope suspend;
        bool prev_suppress = s_suppress_add_component_log;
        s_suppress_add_component_log = true;
        build_demo_scene();
        s_suppress_add_component_log = prev_suppress;
        jce_editor_scene_ensure_rendering_settings(s.scene);
    }

    set_current_scene_path_internal(NULL);
    jce_editor_scene_set_scene_dir("");
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

    /* New scene has no streaming settings — this is effectively a teardown
     * no-op, but keeps the swap contract uniform across all loaders. */
    jce_editor_scene_render_streaming_rebuild();

    LOG_INFO(LOG_TAG, "new default scene created (%d entities)",
             (int)g_entity_order.size());
    return true;
}

void jce_editor_state_shutdown(void)
{
    jce_state_close_bundle_preview();
    g_entity_order.clear(); g_entity_order_gen++;
    g_entity_sidecar.clear();
    s_undo_history.clear();
    s_redo_history.clear();
    s_history_suspend_depth = 0;
    s_history_edit_nesting = 0;
    s_history_outer_edit_pushed_snapshot = false;
    s_history_manual_batch_depth = 0;
    s_transaction.active = false;
    s_transaction.label[0] = '\0';
    s_transaction.before.scene_json.clear();
    s_transaction.before.scene_path.clear();
    if (s.scene) {
        jce_scene_destroy(s.scene);
        s.scene = NULL;
    }
    memset(&s, 0, sizeof(s));
    LOG_INFO(LOG_TAG, "editor state shutdown");
}

/* ── Selection ───────────────────────────────────────────────────── */

void jce_state_select_entity(uint32_t id, bool add_to_selection)
{
    if (!add_to_selection)
        jce_state_clear_selection();

    /* Don't add duplicates. */
    if (jce_state_is_selected(id)) {
        s.focused = id;
        return;
    }
    if (s.selected_count >= JCE_MAX_SELECTED) return;

    s.selected[s.selected_count++] = id;
    s.focused = id;
}

void jce_state_deselect_entity(uint32_t id)
{
    for (int i = 0; i < s.selected_count; i++) {
        if (s.selected[i] == id) {
            /* STABLE removal: shift the tail down by one slot rather
             * than the swap-with-last trick. The previous code did
             *   s.selected[i] = s.selected[--s.selected_count];
             * which silently reordered the selection — visible to the
             * user as the "last" entity flickering into the slot of the
             * one they just deselected, because hierarchy/inspector and
             * outline rendering walk the selection array in index order.
             * Preserving order keeps the focused/last/primary entity
             * stable across deselect operations. */
            int tail = s.selected_count - i - 1;
            if (tail > 0) {
                memmove(&s.selected[i], &s.selected[i + 1],
                        (size_t)tail * sizeof(s.selected[0]));
            }
            s.selected_count--;
            if (s.focused == id)
                s.focused = s.selected_count > 0
                    ? s.selected[s.selected_count - 1] /* fall back to new last (most recent) */
                    : 0;
            return;
        }
    }
}

void jce_state_clear_selection(void)
{
    s.selected_count = 0;
    s.focused = 0;
}

bool jce_state_is_selected(uint32_t id)
{
    for (int i = 0; i < s.selected_count; i++)
        if (s.selected[i] == id) return true;
    return false;
}

uint32_t jce_state_get_focused(void) { return s.focused; }

void jce_state_set_focused(uint32_t id)
{
    /* Re-focus inside an existing multi-selection without disturbing
     * insertion order. Callers (e.g. inspector multi-select detail rows)
     * previously did deselect+select(true) which moved the clicked entry
     * to the tail of the array — visible to the user as the clicked row
     * and the bottom row swapping places. */
    if (id != 0 && !jce_state_is_selected(id)) return;
    s.focused = id;
}

const uint32_t *jce_state_get_selection(int *out_count)
{
    if (out_count) *out_count = s.selected_count;
    return s.selected;
}

/* ── Entity Order / Existence ────────────────────────────────────── */

int jce_state_get_entity_count(void)
{
    return (int)g_entity_order.size();
}

uint32_t jce_state_get_entity_id_by_index(int index)
{
    if (index < 0 || index >= (int)g_entity_order.size())
        return 0;
    return g_entity_order[(size_t)index];
}

bool jce_state_entity_exists(uint32_t id)
{
    if (id == 0 || !s.scene) return false;
    return jce_scene_has_editor_meta(s.scene, (JceEntity)id);
}

/* SAFE liveness check: pure membership test against the editor's known-entity
 * list (no ECS getter), so it never faults on a stale/foreign id — unlike
 * casting an arbitrary id to a flecs handle and calling ecs_get_id on it. */
bool jce_state_entity_alive(uint32_t id)
{
    if (id == 0) return false;
    for (uint32_t e : g_entity_order)
        if (e == id) return true;
    return false;
}

/* Resolve an entity by its display name (EditorMeta name) to a LIVE editor id,
 * or 0 if none.  Iterates only known-live entities, so it is crash-safe and the
 * returned id is always alive — the robust way to bind by name (scene-file ids
 * are NOT the editor's live ids). */
uint32_t jce_state_find_by_name(const char *name)
{
    if (!name || !name[0] || !s.scene) return 0;
    for (uint32_t id : g_entity_order) {
        const char *nm = jce_state_entity_name(id);
        if (nm && std::strcmp(nm, name) == 0) return id;
    }
    return 0;
}

/* Drop entities the RUNTIME destroyed (jce.destroy during Play) from the
 * editor's mirror list + selection.  Without this the hierarchy iterates a
 * stale id and calls ecs_get_parent on a dead flecs handle -> ACCESS_VIOLATION.
 * Uses jce_scene_has_editor_meta (an ecs_has query — safe on a non-alive id,
 * unlike the ecs_get_parent that crashed). Call once per frame during Play. */
void jce_state_prune_dead(void)
{
    if (!s.scene) return;
    /* Single-pass compaction: the old per-dead-entity vector::erase shifted
     * the whole tail each time — O(N x D) on a wave-unload frame (a 2048-
     * entity chunk despawn against a 20k-entry order list = tens of millions
     * of u32 moves).  Two-pointer compact keeps the relative order, moves
     * each survivor at most once, and resizes once: O(N) regardless of how
     * many died this frame. */
    size_t keep = 0;
    for (size_t i = 0; i < g_entity_order.size(); ++i) {
        uint32_t id = g_entity_order[i];
        if (id != 0 && jce_scene_has_editor_meta(s.scene, (JceEntity)id)) {
            if (keep != i) g_entity_order[keep] = id;
            ++keep;                           /* still alive */
        } else {
            jce_state_deselect_entity(id);   /* drop from selection/focus too */
        }
    }
    g_entity_order.resize(keep); g_entity_order_gen++;
    /* Also drop any SELECTED entity that is no longer alive even if it was never
     * in g_entity_order — e.g. a STREAMED chunk entity the user picked in the
     * viewport whose chunk then unloaded.  Without this the gizmo / inspector
     * would dereference a dead id and crash.  Backwards because deselect shifts. */
    for (int i = (int)s.selected_count - 1; i >= 0; --i) {
        uint32_t id = s.selected[i];
        if (id == 0 || !jce_scene_has_editor_meta(s.scene, (JceEntity)id))
            jce_state_deselect_entity(id);
    }
}

/* ── World-streaming hierarchy integration ───────────────────────────
 * Mirror streamed chunk entities into the editor's g_entity_order so they are
 * first-class: listed in the Hierarchy panel and selectable like any object.
 * The world streamer fires these (per chunk load/unload, main thread) — the
 * spawn callback appends the chunk's freshly-spawned ids; the despawn callback
 * removes them (and clears any selection) before they are destroyed. */
/* ── Streamed-chunk grouping + preview-filter SSOT ───────────────────
 * Single source of truth shared by the World Streaming panel and the
 * Hierarchy panel (both read/write through these accessors), all session-
 * local (never serialized into the scene):
 *
 *   g_chunk_entities  : chunk id -> the entity ids the streamer spawned for
 *                       that chunk.  Built from the spawn callback (entity
 *                       ids) + the chunk-state callback (the chunk id),
 *                       which fire back-to-back per chunk load.  The
 *                       Hierarchy panel uses it to group streamed entities
 *                       under one collapsible node per chunk.
 *   g_preview_mode    : RADIUS / ALL / FILTER preview mode.
 *   g_preview_filter  : the set of chunk ids wanted in FILTER mode (the
 *                       per-chunk eye toggles in the panel + hierarchy
 *                       drive this).
 *
 * jce_state_streaming_apply_preview() pushes the current mode + filter into
 * the live preview streamer (jce_world_streamer_set_preview_load). */
static std::unordered_map<uint32_t, std::vector<uint32_t>> g_chunk_entities;
static std::vector<uint32_t>          g_pending_spawn_ids;   /* awaiting chunk id */
static JceStreamPreviewMode           g_preview_mode = JCE_STREAM_PREVIEW_RADIUS;
static std::unordered_set<uint32_t>   g_preview_filter;

void jce_state_streamer_mirror_spawn(const uint64_t *ids, uint32_t count)
{
    if (!ids || count == 0) return;
    g_entity_order.reserve(g_entity_order.size() + count);
    g_pending_spawn_ids.reserve(g_pending_spawn_ids.size() + count);
    for (uint32_t i = 0; i < count; i++) {
        if (!ids[i]) continue;
        g_entity_order.push_back((uint32_t)ids[i]); g_entity_order_gen++;
        /* Stage these ids; the chunk-state callback (fired right after the
         * spawn callback, carrying the chunk id) claims them into the
         * chunk->entities group map. */
        g_pending_spawn_ids.push_back((uint32_t)ids[i]);
    }
}

void jce_state_streamer_mirror_despawn(const uint64_t *ids, uint32_t count)
{
    if (!ids || count == 0) return;
    std::unordered_set<uint32_t> dead;
    dead.reserve(count * 2u);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = (uint32_t)ids[i];
        dead.insert(e);
        jce_state_deselect_entity(e);   /* drop selection before the id dies */
    }
    g_entity_order.erase(
        std::remove_if(g_entity_order.begin(), g_entity_order.end(),
                       [&](uint32_t e) { return dead.find(e) != dead.end(); g_entity_order_gen++; }),
        g_entity_order.end());
}

/* Chunk (un)loaded: maintain the chunk->entities group map.  Fired per chunk
 * load/unload on the main thread:
 *   loaded == true  : claim the ids the spawn callback just staged for THIS
 *                     chunk (spawn fires immediately before chunk-state in
 *                     roster_finalize_apply).
 *   loaded == false : forget this chunk's group (chunk-state false fires
 *                     before the despawn callback removes the ids from
 *                     g_entity_order). */
void jce_state_streamer_chunk_state(uint32_t chunk_id, bool loaded)
{
    if (loaded) {
        if (!g_pending_spawn_ids.empty()) {
            g_chunk_entities[chunk_id] = g_pending_spawn_ids;
            g_pending_spawn_ids.clear();
        }
    } else {
        g_chunk_entities.erase(chunk_id);
    }
}

static void streamer_spawn_cb(const uint64_t *ids, uint32_t count, void *user)
{
    (void)user;
    jce_state_streamer_mirror_spawn(ids, count);
}

static void streamer_despawn_cb(const uint64_t *ids, uint32_t count, void *user)
{
    (void)user;
    jce_state_streamer_mirror_despawn(ids, count);
}

/* Install the spawn/despawn hierarchy hooks on a streamer (editor preview +
 * Play call this after creating their streamer; the runtime never does). */
void jce_state_attach_streamer_hierarchy(JceWorldStreamer *ws)
{
    if (!ws) return;
    jce_world_streamer_set_entity_callbacks(ws, streamer_spawn_cb,
                                            streamer_despawn_cb, NULL);
}

/* ── Preview mode + filter SSOT accessors ─────────────────────────────
 * Both the World Streaming panel and the Hierarchy panel read/write through
 * these so the per-chunk eye toggles in either place stay consistent. */

JceStreamPreviewMode jce_state_streaming_get_preview_mode(void)
{
    return g_preview_mode;
}

void jce_state_streaming_set_preview_mode(JceStreamPreviewMode mode)
{
    g_preview_mode = mode;
    jce_state_streaming_apply_preview();
}

bool jce_state_streaming_filter_contains(uint32_t chunk_id)
{
    return g_preview_filter.find(chunk_id) != g_preview_filter.end();
}

void jce_state_streaming_filter_set(uint32_t chunk_id, bool on)
{
    if (on) g_preview_filter.insert(chunk_id);
    else    g_preview_filter.erase(chunk_id);
    if (g_preview_mode == JCE_STREAM_PREVIEW_FILTER)
        jce_state_streaming_apply_preview();
}

void jce_state_streaming_filter_clear(void)
{
    g_preview_filter.clear();
    if (g_preview_mode == JCE_STREAM_PREVIEW_FILTER)
        jce_state_streaming_apply_preview();
}

/* Replace the whole filter set in one shot (Select-All / In-View helpers). */
void jce_state_streaming_filter_set_all(const uint32_t *ids, uint32_t count)
{
    g_preview_filter.clear();
    if (ids)
        for (uint32_t i = 0; i < count; i++) g_preview_filter.insert(ids[i]);
    if (g_preview_mode == JCE_STREAM_PREVIEW_FILTER)
        jce_state_streaming_apply_preview();
}

uint32_t jce_state_streaming_filter_count(void)
{
    return (uint32_t)g_preview_filter.size();
}

/* Push the current mode + filter into the live preview streamer.  Called on
 * every mode/filter mutation and after a streamer rebuild (so the session
 * intent survives the streamer recreation that each settings edit triggers). */
void jce_state_streaming_apply_preview(void)
{
    JceWorldStreamer *ws = jce_editor_get_world_streamer();
    if (!ws) return;

    if (g_preview_mode == JCE_STREAM_PREVIEW_FILTER) {
        std::vector<uint32_t> ids(g_preview_filter.begin(), g_preview_filter.end());
        jce_world_streamer_set_preview_load(
            ws, JCE_STREAM_PREVIEW_FILTER,
            ids.empty() ? NULL : ids.data(), (uint32_t)ids.size());
    } else {
        jce_world_streamer_set_preview_load(ws, g_preview_mode, NULL, 0);
    }
}

/* ── Chunk-group queries (hierarchy chunk view) ──────────────────────── */

/* Collect the ids of all chunks that currently have spawned entities, into
 * `out` (up to max), ascending.  Returns the count. */
uint32_t jce_state_streaming_group_chunk_ids(uint32_t *out, uint32_t max)
{
    if (!out || max == 0) return 0;
    uint32_t n = 0;
    for (const auto &kv : g_chunk_entities) {
        if (n >= max) break;
        out[n++] = kv.first;
    }
    std::sort(out, out + n);
    return n;
}

/* Number of distinct chunks with at least one spawned entity. */
uint32_t jce_state_streaming_group_count(void)
{
    return (uint32_t)g_chunk_entities.size();
}

/* The entity ids spawned for a chunk (NULL/0 if the chunk has none).  The
 * returned pointer is owned by the state and valid until the next streamer
 * (un)load — copy if you need to retain it. */
const uint32_t *jce_state_streaming_chunk_entities(uint32_t chunk_id,
                                                   uint32_t *out_count)
{
    auto it = g_chunk_entities.find(chunk_id);
    if (it == g_chunk_entities.end() || it->second.empty()) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    if (out_count) *out_count = (uint32_t)it->second.size();
    return it->second.data();
}

/* ── HLOD far-skyline proxy coordination ─────────────────────────────
 * The proxy show/hide swap now lives in the ENGINE
 * (jce_world_streamer_attach_hlod), so the editor (scene-view preview + Play)
 * and the standalone runtime (default_main) share ONE implementation — the
 * shipped exe hides a chunk's cheap far-proxy on cell load exactly like the
 * editor does (no double-draw / z-fight).  The editor keeps ONLY its own extra
 * per-chunk concern here: maintaining the hierarchy chunk->entities group map
 * so streamed cells appear grouped in the Hierarchy panel.  That runs even when
 * no HLOD proxies were baked. */

/* Editor-specific extra chunk callback chained by the engine HLOD coordinator:
 * keep the hierarchy chunk grouping in sync (the engine handles the proxy
 * MeshRenderer toggle itself).  Fired per chunk load/unload, main thread. */
static void hlod_editor_grouping_cb(uint32_t chunk_id, bool loaded, void *user)
{
    (void)user;
    jce_state_streamer_chunk_state(chunk_id, loaded);
}

/* Install the engine HLOD coordinator on the streamer, chaining the editor's
 * hierarchy grouping onto the same chunk-state slot.  The engine builds the
 * chunk-id -> proxy-entity map (by EditorMeta name) and toggles each proxy's
 * MeshRenderer on (un)load; safe no-op when no proxies were baked. */
void jce_state_attach_streamer_hlod(JceWorldStreamer *ws)
{
    if (!ws || !s.scene) return;
    jce_world_streamer_attach_hlod(ws, s.scene, hlod_editor_grouping_cb, NULL);
}

/* Forget the editor's chunk groupings (the engine re-shows the proxies itself
 * when the streamer is destroyed, which happens just before this is called). */
void jce_state_detach_streamer_hlod(void)
{
    /* Streamer is going away: forget all chunk groupings + staged spawn ids so
     * the hierarchy chunk view starts clean on the next rebuild.  The preview
     * MODE + FILTER set are intentionally KEPT (session-local user intent) so a
     * rebuild re-applies them — jce_state_streaming_apply_preview() pushes them
     * back into the fresh streamer. */
    g_chunk_entities.clear();
    g_pending_spawn_ids.clear();
}

/* ── Entity property queries (read-through to ECS) ───────────────── */

const char *jce_state_entity_name(uint32_t id)
{
    if (!s.scene || id == 0) return "";
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    return m ? m->name : "";
}

bool jce_state_entity_enabled(uint32_t id)
{
    if (!s.scene || id == 0) return false;
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    return m ? m->enabled : false;
}

uint32_t jce_state_entity_parent(uint32_t id)
{
    if (!s.scene || id == 0) return 0;
    return (uint32_t)jce_scene_get_parent(s.scene, (JceEntity)id);
}

int jce_state_entity_child_count(uint32_t id)
{
    if (!s.scene || id == 0) return 0;
    return jce_scene_get_child_count(s.scene, (JceEntity)id);
}

int jce_state_entity_children(uint32_t id, uint32_t *out, int max)
{
    if (!s.scene || id == 0 || !out || max <= 0) return 0;
    JceEntity buf[JCE_MAX_CHILDREN];
    int cap = (max < JCE_MAX_CHILDREN) ? max : JCE_MAX_CHILDREN;
    int n = jce_scene_get_children(s.scene, (JceEntity)id, buf, cap);
    for (int i = 0; i < n; i++)
        out[i] = (uint32_t)buf[i];
    return n;
}

JceTagColor jce_state_entity_tag_color(uint32_t id)
{
    if (!s.scene || id == 0) return JCE_TAG_NONE;
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    return m ? (JceTagColor)m->tag_color : JCE_TAG_NONE;
}

const char *jce_state_entity_tag(uint32_t id)
{
    if (!s.scene || id == 0) return "";
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    return m ? m->tag : "";
}

bool jce_state_entity_is_prefab(uint32_t id)
{
    if (!s.scene || id == 0) return false;
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    return m ? m->prefab_instance : false;
}

const char *jce_state_entity_prefab_path(uint32_t id)
{
    if (!s.scene || id == 0) return "";
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    return m ? m->prefab_path : "";
}

/* The editor stores 32-bit ids (audit C5-02), so a round trip through these
 * two DISCARDS the entity generation.  That is a real limitation, not a
 * detail: a selection held across a destroy+recreate can address the new
 * occupant of the slot rather than reporting that the old entity is gone.
 *
 * What changed is that the limitation is now LOCAL.  It used to be paid by
 * the whole engine: jce_scene_resolve_entity guessed "high bits zero means
 * the caller stripped the generation", which is bit-identical to a valid
 * generation-0 handle, so gameplay and network code lost dangling detection
 * for every entity in a fresh scene too.  The engine now always checks the
 * generation, and callers that really do hold a bare index — this one — say
 * so explicitly. */
JceEntity jce_state_to_ecs_entity(uint32_t id)
{
    return jce_scene_entity_from_index(s.scene, id);
}
uint32_t  jce_state_from_ecs_entity(JceEntity e) { return (uint32_t)e; }

/* ── Root entity enumeration ─────────────────────────────────────── */

int jce_state_get_root_count(void)
{
    if (!s.scene) return 0;
    int n = 0;
    for (uint32_t id : g_entity_order) {
        if (jce_scene_get_parent(s.scene, (JceEntity)id) == JCE_ENTITY_INVALID)
            ++n;
    }
    return n;
}

uint32_t jce_state_get_root_id(int index)
{
    if (!s.scene || index < 0) return 0;
    int n = 0;
    for (uint32_t id : g_entity_order) {
        if (jce_scene_get_parent(s.scene, (JceEntity)id) == JCE_ENTITY_INVALID) {
            if (n == index) return id;
            ++n;
        }
    }
    return 0;
}

int jce_state_get_roots(uint32_t *out, int max)
{
    if (!s.scene || !out || max <= 0) return 0;

    /* Root-set cache (large-world editor perf).  Finding the parent-less roots
     * scans all of g_entity_order with one jce_scene_get_parent per entity — an
     * O(all-entities) pass the hierarchy panel runs EVERY frame.  On a 150k-entity
     * scene that alone is ~3 ms/frame of pure editor overhead (the second-largest
     * frame phase after scene_render).  The root SET only changes on a structural
     * edit — create / delete / reparent — so cache it, keyed on the union of the
     * scene's structural_epoch (bumped by reparent / add / remove / any world-
     * matrix edit) and the editor order gen (bumped by every g_entity_order
     * mutation).  Either counter advancing forces a rebuild; the union can only
     * OVER-invalidate (recompute as often as today), never return a stale root.
     * g_entity_order_gen is globally monotonic, so the pair also differs after a
     * scene switch even if the new scene's epoch coincides with the old one. */
    static std::vector<uint32_t> s_roots_cache;
    static uint64_t s_roots_struct = UINT64_MAX;
    static uint64_t s_roots_order  = UINT64_MAX;
    const uint64_t se = jce_scene_get_structural_epoch(s.scene);
    const uint64_t oe = g_entity_order_gen;
    if (s_roots_struct != se || s_roots_order != oe) {
        s_roots_cache.clear();
        for (uint32_t id : g_entity_order)
            if (jce_scene_get_parent(s.scene, (JceEntity)id) == JCE_ENTITY_INVALID)
                s_roots_cache.push_back(id);
        s_roots_struct = se;
        s_roots_order  = oe;
    }

    int n = 0;
    const int cap = (int)s_roots_cache.size();
    for (int i = 0; i < cap && n < max; i++)
        out[n++] = s_roots_cache[i];
    return n;
}

/* ── Entity CRUD ─────────────────────────────────────────────────── */

uint32_t jce_state_create_entity(const char *name, uint32_t parent_id)
{
    HistoryEditScope edit_scope;

    if (!s.scene) return 0;

    /* Pass NULL to flecs so duplicate display names don't conflict. */
    JceEntity e = jce_scene_create_entity(s.scene, NULL);
    if (e == JCE_ENTITY_INVALID) return 0;

    JceEditorMeta meta;
    memset(&meta, 0, sizeof(meta));
    snprintf(meta.name, sizeof(meta.name), "%s", name ? name : "Entity");
    meta.tag[0]          = '\0';
    meta.tag_color       = (uint8_t)JCE_TAG_NONE;
    meta.enabled         = true;
    meta.prefab_instance = false;
    meta.prefab_path[0]  = '\0';
    jce_scene_set_editor_meta(s.scene, e, &meta);

    if (parent_id != 0)
        jce_scene_set_parent(s.scene, e, (JceEntity)parent_id);

    JceTransform t;
    t.position = jce_v3(0.0f, 0.0f, 0.0f);
    t.rotation = jce_q_identity();
    t.scale    = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(s.scene, e, &t);

    uint32_t id = (uint32_t)e;
    g_entity_order.push_back(id); g_entity_order_gen++;
    g_entity_sidecar[id] = EditorEntitySidecar{};
    return id;
}

/* Recursively scrub editor-side bookkeeping (sidecar / order / selection)
 * for `id` and all its descendants. The actual ECS deletion is performed
 * once at the end by the public `jce_state_delete_entity` — flecs cascades
 * the ChildOf relationship automatically. Doing per-node ecs_delete during
 * recursion would race with that cascade and corrupt flecs's tables (the
 * symptom is an AV inside ecs_has the next time we touch a freed id). */
static void scrub_editor_state_recursive(uint32_t id)
{
    if (id == 0 || !s.scene) return;
    if (!jce_state_entity_exists(id)) return;

    JceEntity children[JCE_MAX_CHILDREN];
    int cn = jce_scene_get_children(s.scene, (JceEntity)id,
                                    children, JCE_MAX_CHILDREN);
    for (int i = cn - 1; i >= 0; --i)
        scrub_editor_state_recursive((uint32_t)children[i]);

    jce_state_deselect_entity(id);
    erase_from_order(id);
    g_entity_sidecar.erase(id);
}

void jce_state_delete_entity(uint32_t id)
{
    HistoryEditScope edit_scope;

    if (!s.scene || id == 0) return;
    if (!jce_state_entity_exists(id)) return;

    scrub_editor_state_recursive(id);

    /* Single ecs_delete at the root — flecs cascades to ChildOf descendants. */
    jce_scene_destroy_entity(s.scene, (JceEntity)id);

    /* scrub_editor_state_recursive only walks JCE_MAX_CHILDREN (64) children per
     * node, so a node wider than that (e.g. a benchmark's tens-of-thousands of
     * cubes under one root) leaves the overflow as freed ids in g_entity_order.
     * Prune them, else jce_state_get_roots dereferences a dead id next frame
     * (ecs_get_parent AV). Cheap no-op when nothing leaked. */
    jce_state_prune_dead();
}

/* ── In-editor Performance Benchmark spawn (Profiler "Benchmark" tab) ───────
 * Shares the bulk-spawn machinery the JCE_STRESS_* env path uses (undo-history
 * suspend + per-component log suppression) so a 50k-entity workload spawns fast
 * and without flooding the undo stack.  Kinds mirror the panel enum:
 *   0 Draw Call (grid, unique colour)  1 Instancing (scatter cube)
 *   2 Triangle (scatter sphere)        3 Entity Count (grid + ECS spin)
 *   4 Physics (grid + dynamic rigid body). */
extern "C" int jce_scene_stress_spin_runtime;
extern "C" unsigned int jce_scene_stress_spin_root;
static uint32_t s_bench_root    = 0;
static uint32_t s_bench_spawned = 0;
static int      s_bench_kind    = -1;

static void bench_set_pos(uint32_t e, float x, float y, float z)
{
    JceTransform *t = jce_scene_get_transform(s.scene, (JceEntity)e);
    if (t) { t->position.x = x; t->position.y = y; t->position.z = z; }
}

extern "C" void jce_state_benchmark_clear(void)
{
    jce_state_benchmark_isolate(0);   /* restore any scene we hid for isolation */
    /* jce_state_delete_entity cascades + prunes the mirror (the benchmark's
     * tens-of-thousands of children exceed scrub's JCE_MAX_CHILDREN walk). */
    if (s_bench_root) { jce_state_delete_entity(s_bench_root); s_bench_root = 0; }
    s_bench_spawned = 0;
    s_bench_kind    = -1;
    jce_scene_stress_spin_runtime = 0;
    jce_scene_stress_spin_root    = 0u;
    /* Restore the JCE_PERF_LOG latch instead of hard-off: a plain 0 here
     * permanently silenced the log's phase line for the rest of the session
     * (the env latch never re-arms). */
    jce_perf_phase_set_enabled(std::getenv("JCE_PERF_LOG") != NULL);
}

extern "C" uint32_t jce_state_benchmark_spawned(void) { return s_bench_spawned; }
extern "C" int      jce_state_benchmark_kind(void)    { return s_bench_kind; }

/* Isolated-scene benchmarking: hide everything that is NOT part of the active
 * benchmark so the live stats measure only the workload (the current scene's
 * own meshes/terrain otherwise contaminate the numbers — e.g. meadow's ground
 * collider inflates the physics figure).  Reversible: remembers exactly which
 * components it disabled and re-enables only those. */
struct BenchHidden { uint32_t id; uint64_t mask; };
static std::vector<BenchHidden> s_bench_isolated;   /* exactly what we disabled */

static uint32_t bench_root_of(uint32_t id)
{
    uint32_t r = id, p;
    while ((p = (uint32_t)jce_scene_get_parent(s.scene, (JceEntity)r)) != 0 &&
           p != (uint32_t)JCE_ENTITY_INVALID)
        r = p;
    return r;
}

extern "C" void jce_state_benchmark_isolate(int on)
{
    if (!s.scene) return;
    const uint64_t kHideMask = JCE_COMP_FLAG_MESH_RENDERER | JCE_COMP_FLAG_TERRAIN;
    if (on) {
        if (!s_bench_isolated.empty()) return;            /* already isolated */
        std::vector<uint32_t> ids(g_entity_order);        /* order-independent */
        for (uint32_t id : ids) {
            if (id == 0 || id == s_bench_root) continue;
            if (s_bench_root && bench_root_of(id) == s_bench_root) continue;  /* a benchmark entity */
            const uint64_t f = jce_scene_get_component_flags(s.scene, (JceEntity)id) & kHideMask;
            if (!f) continue;
            jce_scene_set_component_enabled(s.scene, (JceEntity)id, f, false);
            s_bench_isolated.push_back(BenchHidden{ id, f });
        }
    } else {
        for (const BenchHidden &h : s_bench_isolated) {
            if (!jce_scene_has_editor_meta(s.scene, (JceEntity)h.id)) continue;  /* gone */
            jce_scene_set_component_enabled(s.scene, (JceEntity)h.id, h.mask, true);
        }
        s_bench_isolated.clear();
    }
}

extern "C" int jce_state_benchmark_is_isolated(void) { return !s_bench_isolated.empty() ? 1 : 0; }

extern "C" void jce_state_benchmark_spawn(int kind, int count)
{
    jce_state_benchmark_clear();
    if (!s.scene || count <= 0) return;

    HistorySuspendScope suspend;                 /* no per-cube undo entries   */
    const bool prev_suppress = s_suppress_add_component_log;
    s_suppress_add_component_log = true;         /* no per-component log spam   */

    s_bench_root = jce_state_create_entity("Benchmark", 0);
    if (s_bench_root) {
        if (kind == 1 || kind == 2) {            /* Instancing / Triangle: 1 scatter */
            uint32_t sc = jce_state_create_entity("BenchScatter", s_bench_root);
            bench_set_pos(sc, 0.0f, 0.0f, 0.0f);
            JceVegetationScatterComponent vs;
            memset(&vs, 0, sizeof vs);
            vs.visible       = true;
            vs.mesh_shape    = (kind == 2) ? 1u : 0u;   /* sphere : cube */
            float side_m     = (float)sqrt((double)count) * 2.0f;
            vs.area_x        = side_m;
            vs.area_z        = side_m;
            vs.density       = (float)count / (side_m * side_m);
            vs.scale_min     = 1.0f;
            vs.scale_max     = 1.0f;
            vs.max_slope_deg = 90.0f;
            vs.seed          = 1u;
            jce_scene_set_vegetation_scatter(s.scene, (JceEntity)sc, &vs);
            s_bench_spawned  = (uint32_t)count;
        } else {                                 /* Draw Call / Entity / Physics: grid */
            const bool unique = (kind == 0);
            const bool phys   = (kind == 4);
            int side = 1;
            while ((long)side * side * side < count) side++;
            const float spacing = phys ? 1.6f : 1.5f;
            const float origin  = -0.5f * (float)(side - 1) * spacing;
            int spawned = 0;
            for (int x = 0; x < side && spawned < count; ++x)
            for (int y = 0; y < side && spawned < count; ++y)
            for (int z = 0; z < side && spawned < count; ++z) {
                uint32_t c = jce_state_create_entity("b", s_bench_root);
                jce_state_add_component(c, JCE_COMP_FLAG_MESH_RENDERER);
                JceMeshRenderer *mm = jce_scene_get_mesh_renderer(s.scene, (JceEntity)c);
                if (mm && unique) {
                    mm->base_color[0] = 0.15f + 0.7f * (float)((spawned * 13) % 101) / 101.0f;
                    mm->base_color[1] = 0.15f + 0.7f * (float)((spawned * 37) % 103) / 103.0f;
                    mm->base_color[2] = 0.15f + 0.7f * (float)((spawned * 71) % 107) / 107.0f;
                    mm->base_color[3] = 1.0f;
                    /* Vary roughness+metallic, not just base_color: base_color rides
                     * the per-instance tint (i_data4) so the instancers merge it away
                     * (a "unique colour" grid actually collapses to one instanced
                     * batch).  roughness/metallic fold into the material KEY instead,
                     * giving each cube a genuinely distinct material the instancers
                     * cannot merge — so the "Draw Call" workload lives up to its name:
                     * N unbatchable solo submits = the true CPU draw-call submission
                     * floor, the axis Unity/UE draw-call benchmarks actually measure
                     * (a colour-only grid measures instanced throughput instead). */
                    mm->roughness = 0.05f + 0.9f * (float)((spawned * 53) % 991) / 991.0f;
                    mm->metallic  = (float)((spawned * 29) % 251) / 251.0f;
                }
                if (phys) {
                    jce_state_add_component(c, JCE_COMP_FLAG_RIGIDBODY);
                    jce_state_add_component(c, JCE_COMP_FLAG_BOX_COLLIDER);
                    if (JceRigidBodyComponent *rb = jce_scene_get_rigidbody(s.scene, (JceEntity)c)) {
                        rb->body_type = 1; rb->mass = 1.0f;
                    }
                    if (JceBoxColliderComponent *bc = jce_scene_get_box_collider(s.scene, (JceEntity)c)) {
                        bc->size[0] = bc->size[1] = bc->size[2] = 1.0f;
                    }
                }
                bench_set_pos(c, origin + x * spacing, origin + y * spacing, origin + z * spacing);
                ++spawned;
            }
            s_bench_spawned = (uint32_t)spawned;
        }
    }

    s_suppress_add_component_log = prev_suppress;
    s_bench_kind = kind;
    if (kind == 3) {
        jce_scene_stress_spin_runtime = 1;   /* Entity Count = per-frame spin */
        jce_scene_stress_spin_root    = s_bench_root;  /* move ONLY the benchmark's
                                                        * own entities, not the
                                                        * real authored scene */
    }
    jce_perf_phase_set_enabled(1);   /* capture CPU phases for the live readout */
}

void jce_state_rename_entity(uint32_t id, const char *name)
{
    HistoryEditScope edit_scope;

    if (!s.scene || id == 0) return;
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    if (!m) return;
    snprintf(m->name, sizeof(m->name), "%s", name ? name : "");
}

/* ── JCE_STRESS_MOVERS=N harness (DOTS-floor L2 soak) ─────────────────
 * Wiggle the first N stress cubes through the REAL jce_scene_set_transform
 * path every frame — exercising invalidate_entity_world (dirty ring +
 * xform_counter) exactly like gizmo/script movers.  Inert unless both the
 * env and a stress spawn are present. */
static uint32_t s_stress_mover_ids[1024];
static float    s_stress_mover_base_y[1024];
static uint32_t s_stress_mover_count = 0;

void jce_state_stress_record_mover(uint32_t id)
{
    static int s_movers_env = -2;
    if (s_movers_env == -2) {
        const char *mv = std::getenv("JCE_STRESS_MOVERS");
        s_movers_env = (mv && mv[0]) ? atoi(mv) : 0;
        if (s_movers_env > 1024) s_movers_env = 1024;
    }
    if (s_movers_env <= 0) return;
    if (s_stress_mover_count < (uint32_t)s_movers_env)
        s_stress_mover_ids[s_stress_mover_count++] = id;
}

void jce_state_stress_move_tick(float dt)
{
    if (s_stress_mover_count == 0 || !s.scene) return;
    static float s_phase = 0.0f;
    static bool  s_base_taken = false;
    s_phase += dt;
    for (uint32_t i = 0; i < s_stress_mover_count; i++) {
        JceEntity e = (JceEntity)s_stress_mover_ids[i];
        JceTransform *tc = jce_scene_get_transform(s.scene, e);
        if (!tc) continue;
        if (!s_base_taken) s_stress_mover_base_y[i] = tc->position.y;
        JceTransform t = *tc;
        t.position.y = s_stress_mover_base_y[i] +
                       0.5f * sinf(s_phase * 2.0f + (float)i * 0.37f);
        jce_scene_set_transform(s.scene, e, &t);
    }
    s_base_taken = true;
}

void jce_state_set_entity_enabled(uint32_t id, bool enabled)
{
    HistoryEditScope edit_scope;

    if (!s.scene || id == 0) return;
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    if (!m) return;
    m->enabled = enabled;
    /* The flag is mutated in place (no set_ call), so signal the frame-
     * invariance counter for cross-frame render caches (DOTS slice 1). */
    jce_scene_bump_enable_gen(s.scene);

    /* Cascade to children. */
    JceEntity children[JCE_MAX_CHILDREN];
    int cn = jce_scene_get_children(s.scene, (JceEntity)id, children, JCE_MAX_CHILDREN);
    for (int i = 0; i < cn; ++i)
        jce_state_set_entity_enabled((uint32_t)children[i], enabled);
}

void jce_state_set_entity_tag(uint32_t id, const char *tag)
{
    HistoryEditScope edit_scope;

    if (!s.scene || id == 0) return;
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    if (!m) return;
    snprintf(m->tag, sizeof(m->tag), "%s", tag ? tag : "");
}

void jce_state_set_entity_tag_color(uint32_t id, JceTagColor color)
{
    HistoryEditScope edit_scope;

    if (!s.scene || id == 0) return;
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    if (!m) return;
    m->tag_color = (uint8_t)color;
}

void jce_state_set_entity_layer(uint32_t id, int layer)
{
    HistoryEditScope edit_scope;

    if (!s.scene || id == 0) return;
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    if (!m) return;
    if (layer < 0 || layer > 31) layer = 0;
    m->layer = layer;
}

void jce_state_reparent_entity(uint32_t id, uint32_t new_parent)
{
    HistoryEditScope edit_scope;

    if (!s.scene || id == 0 || id == new_parent) return;
    if (!jce_state_entity_exists(id)) return;
    jce_scene_reparent(s.scene, (JceEntity)id,
                       new_parent != 0 ? (JceEntity)new_parent
                                       : JCE_ENTITY_INVALID,
                       true);
}

void jce_state_reorder_sibling(uint32_t entity_id, uint32_t ref_id,
                               bool insert_after)
{
    HistoryEditScope edit_scope;

    if (!s.scene || entity_id == 0 || ref_id == 0 || entity_id == ref_id) return;
    if (jce_scene_get_parent(s.scene, (JceEntity)entity_id) !=
        jce_scene_get_parent(s.scene, (JceEntity)ref_id))
        return;

    auto it_e = std::find(g_entity_order.begin(), g_entity_order.end(), entity_id);
    if (it_e == g_entity_order.end()) return;
    g_entity_order.erase(it_e); g_entity_order_gen++;

    auto it_r = std::find(g_entity_order.begin(), g_entity_order.end(), ref_id);
    if (it_r == g_entity_order.end()) {
        g_entity_order.push_back(entity_id); g_entity_order_gen++;
        return;
    }
    if (insert_after) ++it_r;
    g_entity_order.insert(it_r, entity_id);
}

/* ── Component duplication helper (deep-copies all components) ───── */

static void duplicate_components(JceEntity src, JceEntity dst)
{
    jce_editor_component_defaults_ensure_registered();

    /* Generic registry-driven deep copy: every editor descriptor row whose
     * engine row exposes get/set is copied (descriptor-table order keeps
     * Transform first, exactly like the old explicit chain).
     *
     * Runtime-handle notes (preserved from the old explicit chain):
     *  - VideoPlayer / SequencePlayer: their flecs copy hooks duplicate the
     *    authoring fields only and clear the duplicate's runtime handles, so
     *    a plain set is safe.
     *  - ParticleEmitter / Cloth have NO such hook — their registered
     *    dup_fixup resets the duplicate's runtime bookkeeping so it builds
     *    its OWN emitter/cloth on first tick.
     *  - EditorMeta is skipped: jce_state_duplicate_entity() already set the
     *    "(Copy)" name and copies the meta extras itself. */
    std::vector<uint8_t> tmp;
    int n = jce_editor_component_descriptor_count();
    for (int i = 0; i < n; i++) {
        const JceEditorComponentDescriptor *d =
            jce_editor_component_descriptor_at(i);
        if (!d || d->slot == JCE_COMP_FLAG_EDITOR_META) continue;
        int cid = d->comp_id;
        if (cid == JCE_COMP_ID_INVALID) continue;
        if (!jce_scene_has_comp(s.scene, src, cid)) continue;
        uint32_t sz = 0;
        void *p = jce_scene_get_comp(s.scene, src, cid, &sz);
        if (!p || sz == 0) continue;   /* row without raw accessor (Light group) */
        /* Snapshot before the set: adding a component to dst can move flecs
         * table rows, which may invalidate the src pointer mid-copy. */
        tmp.assign((const uint8_t *)p, (const uint8_t *)p + sz);
        if (!jce_scene_set_comp(s.scene, dst, cid, tmp.data())) continue;
        if (d->dup_fixup) d->dup_fixup(s.scene, dst);
    }
}

uint32_t jce_state_duplicate_entity(uint32_t id)
{
    HistoryEditScope edit_scope;

    if (!s.scene || id == 0 || !jce_state_entity_exists(id)) return 0;

    JceEditorMeta *src_meta = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    if (!src_meta) return 0;

    char dup_name[64];
    snprintf(dup_name, sizeof(dup_name), "%s (Copy)", src_meta->name);

    uint32_t parent_id = (uint32_t)jce_scene_get_parent(s.scene, (JceEntity)id);
    uint32_t dup = jce_state_create_entity(dup_name, parent_id);
    if (dup == 0) return 0;

    /* Copy editor-meta extras (name already set). */
    JceEditorMeta *dup_meta = jce_scene_get_editor_meta(s.scene, (JceEntity)dup);
    src_meta = jce_scene_get_editor_meta(s.scene, (JceEntity)id);  /* refresh */
    if (dup_meta && src_meta) {
        dup_meta->tag_color       = src_meta->tag_color;
        dup_meta->enabled         = src_meta->enabled;
        snprintf(dup_meta->tag, sizeof(dup_meta->tag), "%s", src_meta->tag);
        dup_meta->prefab_instance = src_meta->prefab_instance;
        snprintf(dup_meta->prefab_path, sizeof(dup_meta->prefab_path),
                 "%s", src_meta->prefab_path);
    }

    /* Deep-copy all components from src → dup (overwrites the identity
     * Transform that create_entity installed). */
    duplicate_components((JceEntity)id, (JceEntity)dup);

    /* Recursively duplicate children, reparenting under dup. */
    JceEntity src_children[JCE_MAX_CHILDREN];
    int cn = jce_scene_get_children(s.scene, (JceEntity)id, src_children, JCE_MAX_CHILDREN);
    for (int i = 0; i < cn; ++i) {
        uint32_t child_dup = jce_state_duplicate_entity((uint32_t)src_children[i]);
        if (child_dup != 0)
            jce_scene_set_parent(s.scene, (JceEntity)child_dup, (JceEntity)dup);
    }

    return dup;
}

/* ── Component management ────────────────────────────────────────── */

void jce_state_add_component_id(uint32_t entity_id, int comp_id)
{
    HistoryEditScope edit_scope;

    if (!s.scene || entity_id == 0) return;
    JceEntity e = (JceEntity)entity_id;

    /* Registry route: dense comp_id -> descriptor row -> registered
     * default-init.  The per-component default bodies (and the Net*
     * ensure-NetworkObject hooks) live in
     * jce_editor_component_defaults.cpp. */
    jce_editor_component_defaults_ensure_registered();
    const JceEditorComponentDescriptor *desc =
        jce_editor_component_find_by_id(comp_id);
    if (!desc || !desc->add_default) return;
    if (jce_scene_has_comp(s.scene, e, comp_id)) return;

    desc->add_default(s.scene, e);

    if (!s_suppress_add_component_log) {
        LOG_INFO(LOG_TAG, "add component %s to entity %u",
                 desc->display_name, entity_id);
    }
}

void jce_state_add_component(uint32_t entity_id, uint64_t comp_flag)
{
    /* Legacy-flag entry point (hierarchy/scene-view shortcuts still hold
     * JCE_COMP_FLAG_* values); resolves to the dense-id path. */
    const JceEditorComponentDescriptor *desc =
        jce_editor_component_find(comp_flag);
    if (!desc) return;
    jce_state_add_component_id(entity_id, desc->comp_id);
}

void jce_state_remove_component_id(uint32_t entity_id, int comp_id)
{
    HistoryEditScope edit_scope;

    if (!s.scene || entity_id == 0) return;
    JceEntity e = (JceEntity)entity_id;

    /* Registry route: dense comp_id -> descriptor row.  Per-row
     * pre_remove hooks carry the runtime cleanup that used to be
     * special-cased here (Cloth destroys its live cloth handle). */
    jce_editor_component_defaults_ensure_registered();
    const JceEditorComponentDescriptor *desc =
        jce_editor_component_find_by_id(comp_id);
    if (!desc || desc->comp_id == JCE_COMP_ID_INVALID) return;

    if (desc->pre_remove) desc->pre_remove(s.scene, e);
    jce_scene_remove_comp(s.scene, e, desc->comp_id);

    LOG_INFO(LOG_TAG, "remove component %s from entity %u",
             desc->display_name, entity_id);
}

void jce_state_remove_component(uint32_t entity_id, uint64_t comp_flag)
{
    const JceEditorComponentDescriptor *desc =
        jce_editor_component_find(comp_flag);
    if (!desc) return;
    jce_state_remove_component_id(entity_id, desc->comp_id);
}

const char *jce_comp_flag_display_name(uint64_t comp_flag)
{
    const JceEditorComponentDescriptor *desc =
        jce_editor_component_find(comp_flag);
    if (desc)
        return desc->display_name;

    switch (comp_flag) {
    case JCE_COMP_FLAG_TRANSFORM:            return "Transform";
    case JCE_COMP_FLAG_MESH_RENDERER:        return "Mesh Renderer";
    case JCE_COMP_FLAG_CAMERA:               return "Camera";
    case JCE_COMP_FLAG_DIR_LIGHT:            return "Directional Light";
    case JCE_COMP_FLAG_POINT_LIGHT:          return "Point Light";
    case JCE_COMP_FLAG_SPOT_LIGHT:           return "Spot Light";
    case JCE_COMP_FLAG_SKYBOX:               return "Skybox";
    case JCE_COMP_FLAG_SPRITE_RENDERER:      return "Sprite Renderer";
    case JCE_COMP_FLAG_SPRITE_ANIMATOR:      return "Sprite Animator";
    case JCE_COMP_FLAG_ANIMATOR:             return "Animator";
    case JCE_COMP_FLAG_SKELETAL_ANIMATOR:    return "Skeletal Animator";
    case JCE_COMP_FLAG_CONSTRAINT:           return "Constraint";
    case JCE_COMP_FLAG_RIGIDBODY:            return "Rigidbody";
    case JCE_COMP_FLAG_RIGIDBODY_2D:         return "Rigidbody 2D";
    case JCE_COMP_FLAG_BOX_COLLIDER:         return "Box Collider";
    case JCE_COMP_FLAG_SPHERE_COLLIDER:      return "Sphere Collider";
    case JCE_COMP_FLAG_CHARACTER_CONTROLLER: return "Character Controller";
    case JCE_COMP_FLAG_AUDIO_SOURCE:         return "Audio Source";
    case JCE_COMP_FLAG_SCRIPT:               return "Script";
    case JCE_COMP_FLAG_PARTICLE_EMITTER:     return "Particle Emitter";
    case JCE_COMP_FLAG_BEHAVIOR_TREE:        return "Behavior Tree";
    case JCE_COMP_FLAG_EDITOR_META:          return "Editor Meta";
    case JCE_COMP_FLAG_TERRAIN:              return "Terrain";
    case JCE_COMP_FLAG_LOD_GROUP:            return "LOD Group";
    case JCE_COMP_FLAG_VIRTUAL_CAMERA:       return "Virtual Camera";
    case JCE_COMP_FLAG_TRIGGER_VOLUME:       return "Trigger Volume";
    case JCE_COMP_FLAG_CAPSULE_COLLIDER:     return "Capsule Collider";
    case JCE_COMP_FLAG_MESH_COLLIDER:        return "Mesh Collider";
    case JCE_COMP_FLAG_COLLIDER_2D:          return "Collider 2D";
    case JCE_COMP_FLAG_TRAIL_RENDERER:       return "Trail Renderer";
    case JCE_COMP_FLAG_LINE_RENDERER:        return "Line Renderer";
    case JCE_COMP_FLAG_REFLECTION_PROBE:     return "Reflection Probe";
    case JCE_COMP_FLAG_DECAL:                return "Decal Projector";
    case JCE_COMP_FLAG_LIGHT_PROBE_GROUP:    return "Light Probe Group";
    case JCE_COMP_FLAG_AUDIO_LISTENER:       return "Audio Listener";
    case JCE_COMP_FLAG_AUDIO_REVERB_ZONE:    return "Audio Reverb Zone";
    case JCE_COMP_FLAG_AUDIO_OCCLUSION:      return "Audio Occlusion";
    case JCE_COMP_FLAG_SPAWN_MANAGER:        return "Spawn Manager";
    case JCE_COMP_FLAG_WEAPON:               return "Weapon";
    case JCE_COMP_FLAG_SAVE_POINT:           return "Save Point";
    case JCE_COMP_FLAG_WHEEL_COLLIDER:       return "Wheel Collider";
    case JCE_COMP_FLAG_CONSTANT_FORCE:       return "Constant Force";
    case JCE_COMP_FLAG_CONFIGURABLE_JOINT:   return "Configurable Joint";
    case JCE_COMP_FLAG_JOINT_2D:             return "Joint 2D";
    case JCE_COMP_FLAG_BILLBOARD_RENDERER:   return "Billboard Renderer";
    case JCE_COMP_FLAG_CANVAS:               return "Canvas";
    case JCE_COMP_FLAG_CANVAS_GROUP:         return "Canvas Group";
    case JCE_COMP_FLAG_LAYOUT_GROUP:         return "Layout Group";
    case JCE_COMP_FLAG_UI_IMAGE:             return "UI Image";
    case JCE_COMP_FLAG_UI_TEXT:              return "UI Text";
    case JCE_COMP_FLAG_UI_BUTTON:            return "UI Button";
    case JCE_COMP_FLAG_CLOTH:                return "Cloth";
    case JCE_COMP_FLAG_NETWORK_OBJECT:       return "Network Object";
    case JCE_COMP_FLAG_NET_TRANSFORM:        return "Network Transform";
    case JCE_COMP_FLAG_NET_ANIMATOR:         return "Network Animator";
    case JCE_COMP_FLAG_NET_RIGIDBODY:        return "Network Rigidbody";
    case JCE_COMP_FLAG_TILEMAP:              return "Tilemap";
    case JCE_COMP_FLAG_TILEMAP_COLLIDER_2D:  return "Tilemap Collider 2D";
    case JCE_COMP_FLAG_AVATAR:               return "Avatar";
    case JCE_COMP_FLAG_VOLUME:               return "Volume";
    case JCE_COMP_FLAG_OCCLUSION_PORTAL:     return "Occlusion Portal";
    default:                                 return "Unknown";
    }
}

const char *jce_comp_flag_i18n_key(uint64_t comp_flag)
{
    const JceEditorComponentDescriptor *desc =
        jce_editor_component_find(comp_flag);
    if (desc)
        return desc->i18n_key;

    switch (comp_flag) {
    case JCE_COMP_FLAG_TRANSFORM:            return "comp.transform";
    case JCE_COMP_FLAG_MESH_RENDERER:        return "comp.meshRenderer";
    case JCE_COMP_FLAG_CAMERA:               return "comp.camera";
    case JCE_COMP_FLAG_DIR_LIGHT:            return "comp.dirLight";
    case JCE_COMP_FLAG_POINT_LIGHT:          return "comp.pointLight";
    case JCE_COMP_FLAG_SPOT_LIGHT:           return "comp.spotLight";
    case JCE_COMP_FLAG_SKYBOX:               return "comp.skybox";
    case JCE_COMP_FLAG_SPRITE_RENDERER:      return "comp.spriteRenderer";
    case JCE_COMP_FLAG_SPRITE_ANIMATOR:      return "comp.spriteAnimator";
    case JCE_COMP_FLAG_ANIMATOR:             return "comp.animator";
    case JCE_COMP_FLAG_SKELETAL_ANIMATOR:    return "comp.skeletalAnimator";
    case JCE_COMP_FLAG_CONSTRAINT:           return "comp.constraint";
    case JCE_COMP_FLAG_RIGIDBODY:            return "comp.rigidbody";
    case JCE_COMP_FLAG_RIGIDBODY_2D:         return "comp.rigidbody2d";
    case JCE_COMP_FLAG_BOX_COLLIDER:         return "comp.boxCollider";
    case JCE_COMP_FLAG_SPHERE_COLLIDER:      return "comp.sphereCollider";
    case JCE_COMP_FLAG_CHARACTER_CONTROLLER: return "comp.characterController";
    case JCE_COMP_FLAG_AUDIO_SOURCE:         return "comp.audioSource";
    case JCE_COMP_FLAG_SCRIPT:               return "comp.script";
    case JCE_COMP_FLAG_PARTICLE_EMITTER:     return "comp.particleEmitter";
    case JCE_COMP_FLAG_BEHAVIOR_TREE:        return "comp.behaviorTree";
    case JCE_COMP_FLAG_EDITOR_META:          return "comp.editorMeta";
    case JCE_COMP_FLAG_TERRAIN:              return "comp.terrain";
    case JCE_COMP_FLAG_LOD_GROUP:            return "comp.lodGroup";
    case JCE_COMP_FLAG_VIRTUAL_CAMERA:       return "comp.virtualCamera";
    case JCE_COMP_FLAG_TRIGGER_VOLUME:       return "comp.triggerVolume";
    case JCE_COMP_FLAG_CAPSULE_COLLIDER:     return "comp.capsuleCollider";
    case JCE_COMP_FLAG_MESH_COLLIDER:        return "comp.meshCollider";
    case JCE_COMP_FLAG_COLLIDER_2D:          return "comp.collider2d";
    case JCE_COMP_FLAG_TRAIL_RENDERER:       return "comp.lineRenderer"; /* fallback */
    case JCE_COMP_FLAG_LINE_RENDERER:        return "comp.lineRenderer";
    case JCE_COMP_FLAG_REFLECTION_PROBE:     return "comp.reflectionProbe";
    case JCE_COMP_FLAG_DECAL:                return "comp.decal";
    case JCE_COMP_FLAG_LIGHT_PROBE_GROUP:    return "comp.lightProbeGroup";
    case JCE_COMP_FLAG_AUDIO_LISTENER:       return "comp.audioListener";
    case JCE_COMP_FLAG_AUDIO_REVERB_ZONE:    return "comp.audioReverbZone";
    case JCE_COMP_FLAG_AUDIO_OCCLUSION:      return "comp.audioOcclusion";
    case JCE_COMP_FLAG_SPAWN_MANAGER:        return "comp.spawnManager";
    case JCE_COMP_FLAG_WEAPON:               return "comp.weapon";
    case JCE_COMP_FLAG_SAVE_POINT:           return "comp.savePoint";
    case JCE_COMP_FLAG_WHEEL_COLLIDER:       return "comp.wheelCollider";
    case JCE_COMP_FLAG_CONSTANT_FORCE:       return "comp.constantForce";
    case JCE_COMP_FLAG_CONFIGURABLE_JOINT:   return "comp.configurableJoint";
    case JCE_COMP_FLAG_JOINT_2D:             return "comp.joint2d";
    case JCE_COMP_FLAG_BILLBOARD_RENDERER:   return "comp.billboardRenderer";
    case JCE_COMP_FLAG_CANVAS:               return "comp.canvas";
    case JCE_COMP_FLAG_CANVAS_GROUP:         return "comp.canvasGroup";
    case JCE_COMP_FLAG_LAYOUT_GROUP:         return "comp.layoutGroup";
    case JCE_COMP_FLAG_UI_IMAGE:             return "comp.uiImage";
    case JCE_COMP_FLAG_UI_TEXT:              return "comp.uiText";
    case JCE_COMP_FLAG_UI_BUTTON:            return "comp.uiButton";
    case JCE_COMP_FLAG_CLOTH:                return "comp.cloth";
    case JCE_COMP_FLAG_NET_TRANSFORM:        return "comp.netTransform";
    case JCE_COMP_FLAG_NET_ANIMATOR:         return "comp.netAnimator";
    case JCE_COMP_FLAG_NET_RIGIDBODY:        return "comp.netRigidbody";
    case JCE_COMP_FLAG_TILEMAP:              return "comp.tilemap";
    case JCE_COMP_FLAG_TILEMAP_COLLIDER_2D:  return "comp.tilemapCollider2d";
    case JCE_COMP_FLAG_AVATAR:               return "comp.avatar";
    case JCE_COMP_FLAG_VOLUME:               return "comp.volume";
    case JCE_COMP_FLAG_OCCLUSION_PORTAL:     return "comp.occlusionPortal";
    default:                                 return NULL;
    }
}

/* ── Mode Accessors ──────────────────────────────────────────────── */

void          jce_state_set_edit_mode(JceEditMode m)       { s.edit_mode = m; }
JceEditMode   jce_state_get_edit_mode(void)                { return s.edit_mode; }

/* Gizmo mode/space/pivot persist per user (editor-session.json) — the
 * save-on-set / load-on-init pair keeps the tool selection across runs. */
void          jce_state_set_gizmo_mode(JceGizmoMode m)     { s.gizmo_mode = m; jce_editor_ui_state_save_int("gizmo.mode", (int)m); }
JceGizmoMode  jce_state_get_gizmo_mode(void)               { return s.gizmo_mode; }

void          jce_state_set_gizmo_space(JceGizmoSpace sp)  { s.gizmo_space = sp; jce_editor_ui_state_save_int("gizmo.space", (int)sp); }
JceGizmoSpace jce_state_get_gizmo_space(void)              { return s.gizmo_space; }

void          jce_state_set_gizmo_pivot(JceGizmoPivot p)   { s.gizmo_pivot = p; jce_editor_ui_state_save_int("gizmo.pivot", (int)p); }
JceGizmoPivot jce_state_get_gizmo_pivot(void)              { return s.gizmo_pivot; }
void          jce_state_set_pivot_edit_mode(bool enabled)  { s.pivot_edit_mode = enabled; }
bool          jce_state_get_pivot_edit_mode(void)          { return s.pivot_edit_mode; }

/* Persist view_mode, show_grid and gizmo snap increments via the config
 * singleton (snap increments -> ~/.jce/editor-preferences.json).  view_mode
 * and show_grid are per-PROJECT view state (a debug view left on in project
 * A must not greet project B), so they ALSO mirror into the project store
 * when one is open; the global copy is kept as the pre-project-open fallback
 * (cold boot, or working with no project). */
static void persist_render_settings(void)
{
    JceEditorConfig ecfg;
    jce_editor_config_load(&ecfg);
    ecfg.view_mode = (int)s.view_mode;
    ecfg.show_grid = s.show_grid;
    ecfg.gizmo_snap_translate = s_gizmo_snap_translate;
    ecfg.gizmo_snap_rotate    = s_gizmo_snap_rotate;
    ecfg.gizmo_snap_scale     = s_gizmo_snap_scale;
    jce_editor_config_save(&ecfg);

    if (jce_editor_pstate_active()) {
        jce_editor_pstate_set_int("view.mode", (int)s.view_mode);
        jce_editor_pstate_set_int("view.show_grid", s.show_grid ? 1 : 0);
    }
}

/* Apply the opened project's view mode / grid toggle over the global
 * fallback.  Called from the project-open seam (set_current_project_root)
 * after the project store follows the new root.  Absent keys leave the
 * current (global-loaded) values untouched, and seed the project store so
 * the next switch is authoritative. */
void jce_state_apply_project_view_settings(void)
{
    if (!jce_editor_pstate_active()) return;
    int vm = jce_editor_pstate_get_int("view.mode", -1);
    int sg = jce_editor_pstate_get_int("view.show_grid", -1);
    if (vm < 0 && sg < 0) {
        /* First time this project is seen: seed from the current values. */
        jce_editor_pstate_set_int("view.mode", (int)s.view_mode);
        jce_editor_pstate_set_int("view.show_grid", s.show_grid ? 1 : 0);
        return;
    }
    if (vm >= JCE_VIEW_SHADED && vm <= JCE_VIEW_AO)
        s.view_mode = (JceSceneViewMode)vm;
    if (sg >= 0)
        s.show_grid = (sg != 0);
    /* Mirror into the global fallback so a later no-project window matches. */
    JceEditorConfig ecfg;
    jce_editor_config_load(&ecfg);
    ecfg.view_mode = (int)s.view_mode;
    ecfg.show_grid = s.show_grid;
    jce_editor_config_save(&ecfg);
}

void              jce_state_set_view_mode(JceSceneViewMode m)  { s.view_mode = m; persist_render_settings(); }
JceSceneViewMode  jce_state_get_view_mode(void)                { return s.view_mode; }

bool  jce_state_get_show_grid(void)          { return s.show_grid; }
void  jce_state_set_show_grid(bool show)     { s.show_grid = show; persist_render_settings(); }

float jce_state_get_gizmo_snap_translate(void) { return s_gizmo_snap_translate; }
float jce_state_get_gizmo_snap_rotate(void)    { return s_gizmo_snap_rotate; }
float jce_state_get_gizmo_snap_scale(void)     { return s_gizmo_snap_scale; }
bool  jce_state_get_gizmo_snap_enabled(void)   { return s_gizmo_snap_enabled; }
void  jce_state_set_gizmo_snap_enabled(bool v)
{
    s_gizmo_snap_enabled = v;
    jce_editor_ui_state_save_int("gizmo.snap_enabled", v ? 1 : 0);
}

void  jce_state_set_gizmo_snap_translate(float v)
{
    if (v < 0.001f) v = 0.001f;
    s_gizmo_snap_translate = v;
    persist_render_settings();
}
void  jce_state_set_gizmo_snap_rotate(float v)
{
    if (v < 0.001f) v = 0.001f;
    s_gizmo_snap_rotate = v;
    persist_render_settings();
}
void  jce_state_set_gizmo_snap_scale(float v)
{
    if (v < 0.001f) v = 0.001f;
    s_gizmo_snap_scale = v;
    persist_render_settings();
}

/* Standalone overlay toggles (not part of the show-flags bitmask) —
 * persisted per user under the same "sceneview.show.*" namespace. */
static bool s_show_physics_debug = false;
bool  jce_state_get_show_physics_debug(void)  { return s_show_physics_debug; }
void  jce_state_set_show_physics_debug(bool v)
{
    s_show_physics_debug = v;
    jce_editor_ui_state_save_int("sceneview.show.physics_debug", v ? 1 : 0);
}

static bool s_show_joint_gizmos = true;
bool  jce_state_get_show_joint_gizmos(void)   { return s_show_joint_gizmos; }
void  jce_state_set_show_joint_gizmos(bool v)
{
    s_show_joint_gizmos = v;
    jce_editor_ui_state_save_int("sceneview.show.joints", v ? 1 : 0);
}

static bool s_show_cloth_gizmos = true;
bool  jce_state_get_show_cloth_gizmos(void)   { return s_show_cloth_gizmos; }
void  jce_state_set_show_cloth_gizmos(bool v)
{
    s_show_cloth_gizmos = v;
    jce_editor_ui_state_save_int("sceneview.show.cloth", v ? 1 : 0);
}

/* World-streaming preview (session-local; never persisted — a freshly
 * opened editor must not mutate the hierarchy until the user opts in). */
static bool s_streaming_preview = false;
bool  jce_state_get_streaming_preview(void)   { return s_streaming_preview; }
void  jce_state_set_streaming_preview(bool v) { s_streaming_preview = v; }

/* Show flags bitmask. Defaults: gizmos + light icons + camera icons +
 * skybox + world axis on; rest off. */
static uint32_t s_show_flags =
      JCE_SHOW_FLAG_GIZMOS
    | JCE_SHOW_FLAG_LIGHT_ICONS
    | JCE_SHOW_FLAG_CAMERA_ICONS
    | JCE_SHOW_FLAG_SKYBOX
    | JCE_SHOW_FLAG_WORLD_AXIS
    | JCE_SHOW_FLAG_PARTICLE_ICONS   /* particle source gizmos ON so emitters are selectable */
    | JCE_SHOW_FLAG_UI;   /* scene-view Canvas overlay ON like Unity */

/* Persisted show flags, keyed by NAME rather than bit position so the
 * mask can be renumbered without scrambling saved sessions.  Flags with
 * no renderer/overlay consumer yet (colliders, bounding boxes, stats
 * overlay) are deliberately absent: persisting a bit nothing draws
 * would only freeze dead state into every session file. */
static const struct { uint32_t bit; const char *key; } k_show_flag_keys[] = {
    { JCE_SHOW_FLAG_GIZMOS,       "sceneview.show.gizmos"       },
    { JCE_SHOW_FLAG_LIGHT_ICONS,  "sceneview.show.light_icons"  },
    { JCE_SHOW_FLAG_CAMERA_ICONS, "sceneview.show.camera_icons" },
    { JCE_SHOW_FLAG_SKYBOX,       "sceneview.show.skybox"       },
    { JCE_SHOW_FLAG_WORLD_AXIS,   "sceneview.show.world_axis"   },
    { JCE_SHOW_FLAG_NAVMESH,      "sceneview.show.navmesh"      },
    { JCE_SHOW_FLAG_STREAMING,    "sceneview.show.streaming"    },
    { JCE_SHOW_FLAG_UI,           "sceneview.show.ui"           },
    { JCE_SHOW_FLAG_PARTICLE_ICONS, "sceneview.show.particle_icons" },
};

static void save_named_show_flags(uint32_t affected_mask)
{
    for (const auto &e : k_show_flag_keys)
        if (affected_mask & e.bit)
            jce_editor_ui_state_save_int(e.key,
                                         (s_show_flags & e.bit) ? 1 : 0);
}

uint32_t jce_state_get_show_flags(void)            { return s_show_flags; }
void     jce_state_set_show_flags(uint32_t flags)
{
    s_show_flags = flags;
    save_named_show_flags(0xFFFFFFFFu);   /* bulk set (All On/Off/Defaults) */
}
bool     jce_state_show_flag(JceShowFlag f)        { return (s_show_flags & (uint32_t)f) != 0; }
void     jce_state_set_show_flag(JceShowFlag f, bool on)
{
    if (on) s_show_flags |=  (uint32_t)f;
    else    s_show_flags &= ~(uint32_t)f;
    save_named_show_flags((uint32_t)f);
}

bool  jce_state_get_2d_mode(void)            { return s.is_2d_mode; }
void  jce_state_set_2d_mode(bool is_2d)
{
    s.is_2d_mode = is_2d;
    jce_editor_ui_state_save_int("sceneview.mode_2d", is_2d ? 1 : 0);
}

/* Load-once counterpart of the save-on-set persistence above.  Fallbacks
 * are the code defaults so a fresh session file changes nothing. */
static void load_persisted_view_toggles(void)
{
    s.gizmo_mode  = (JceGizmoMode)jce_editor_ui_state_load_int(
        "gizmo.mode",  (int)s.gizmo_mode,  JCE_GIZMO_TRANSLATE, JCE_GIZMO_SCALE);
    s.gizmo_space = (JceGizmoSpace)jce_editor_ui_state_load_int(
        "gizmo.space", (int)s.gizmo_space, JCE_GIZMO_LOCAL, JCE_GIZMO_WORLD);
    s.gizmo_pivot = (JceGizmoPivot)jce_editor_ui_state_load_int(
        "gizmo.pivot", (int)s.gizmo_pivot, JCE_GIZMO_PIVOT, JCE_GIZMO_CENTER);
    s_gizmo_snap_enabled = jce_editor_ui_state_load_int(
        "gizmo.snap_enabled", s_gizmo_snap_enabled ? 1 : 0, 0, 1) != 0;
    s.is_2d_mode = jce_editor_ui_state_load_int(
        "sceneview.mode_2d", s.is_2d_mode ? 1 : 0, 0, 1) != 0;

    s_show_physics_debug = jce_editor_ui_state_load_int(
        "sceneview.show.physics_debug", s_show_physics_debug ? 1 : 0, 0, 1) != 0;
    s_show_joint_gizmos = jce_editor_ui_state_load_int(
        "sceneview.show.joints", s_show_joint_gizmos ? 1 : 0, 0, 1) != 0;
    s_show_cloth_gizmos = jce_editor_ui_state_load_int(
        "sceneview.show.cloth", s_show_cloth_gizmos ? 1 : 0, 0, 1) != 0;

    for (const auto &e : k_show_flag_keys) {
        bool on = jce_editor_ui_state_load_int(
            e.key, (s_show_flags & e.bit) ? 1 : 0, 0, 1) != 0;
        if (on) s_show_flags |=  e.bit;
        else    s_show_flags &= ~e.bit;
    }
}

bool  jce_state_get_live_preview(void)       { return s.live_preview; }
void  jce_state_set_live_preview(bool on)    { s.live_preview = on; }

void jce_state_set_scene(JceScene *scene) { s.scene = scene; }
JceScene *jce_state_get_scene(void) { return s.scene; }


bool jce_state_is_scene_modified(void) { return s.scene_modified; }
void jce_state_mark_scene_modified(void) { s.scene_modified = true; }
void jce_state_clear_scene_modified(void) { s.scene_modified = false; }
