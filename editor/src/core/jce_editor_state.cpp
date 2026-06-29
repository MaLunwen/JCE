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
#include "ui/jce_editor_panels.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

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
 * (Ctrl still forces snap momentarily). Session-scoped for now. */
static bool  s_gizmo_snap_enabled   = false;

/* ── Internal helpers ─────────────────────────────────────────────── */

static void erase_from_order(uint32_t id)
{
    auto it = std::find(g_entity_order.begin(), g_entity_order.end(), id);
    if (it != g_entity_order.end())
        g_entity_order.erase(it);
}

static void rebuild_order_cb(JceScene * /*scene*/, JceEntity e, void *user_data)
{
    auto *out = static_cast<std::vector<uint32_t> *>(user_data);
    out->push_back((uint32_t)e);
}

void rebuild_entity_order_from_ecs(void)
{
    g_entity_order.clear();
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
    g_entity_order.clear();
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
            const long n = strtol(env, nullptr, 10);
            if (n > 0 && n <= 200000) {
                LOG_INFO(LOG_TAG, "stress test: spawning %ld cubes", n);
                uint32_t stress_root = jce_state_create_entity("Stress Cubes", root);

                /* Cube root of N rounded up so the side length covers all. */
                int side = 1;
                while ((long)side * side * side < n) side++;
                const float spacing = 1.5f;
                const float origin  = -0.5f * (side - 1) * spacing;

                s_suppress_add_component_log = true;
                long spawned = 0;
                for (int x = 0; x < side && spawned < n; x++) {
                    for (int y = 0; y < side && spawned < n; y++) {
                        for (int z = 0; z < side && spawned < n; z++) {
                            uint32_t c = jce_state_create_entity("c", stress_root);
                            jce_state_add_component(c, JCE_COMP_FLAG_MESH_RENDERER);
                            demo_set_position(c,
                                              origin + x * spacing,
                                              origin + y * spacing,
                                              origin + z * spacing);
                            spawned++;
                        }
                    }
                }
                s_suppress_add_component_log = false;
                LOG_INFO(LOG_TAG, "stress test: spawned %ld cubes (%dx%dx%d grid)",
                         spawned, side, side, side);
            }
        }
    }
}

/* ── Init / Shutdown ─────────────────────────────────────────────── */

void jce_editor_state_init(bool with_demo_scene)
{
    memset(&s, 0, sizeof(s));
    g_entity_order.clear();
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

    /* Load persisted render settings from .jce/editor-config.json. */
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
    g_entity_order.clear();
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
    for (size_t i = 0; i < g_entity_order.size(); ) {
        uint32_t id = g_entity_order[i];
        if (id != 0 && jce_scene_has_editor_meta(s.scene, (JceEntity)id)) {
            ++i;                              /* still alive */
        } else {
            jce_state_deselect_entity(id);   /* drop from selection/focus too */
            g_entity_order.erase(g_entity_order.begin() + (long)i);
        }
    }
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
        g_entity_order.push_back((uint32_t)ids[i]);
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
                       [&](uint32_t e) { return dead.find(e) != dead.end(); }),
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

JceEntity jce_state_to_ecs_entity(uint32_t id)   { return (JceEntity)id; }
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
    int n = 0;
    for (uint32_t id : g_entity_order) {
        if (n >= max) break;
        if (jce_scene_get_parent(s.scene, (JceEntity)id) == JCE_ENTITY_INVALID)
            out[n++] = id;
    }
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
    g_entity_order.push_back(id);
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
}

void jce_state_rename_entity(uint32_t id, const char *name)
{
    HistoryEditScope edit_scope;

    if (!s.scene || id == 0) return;
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    if (!m) return;
    snprintf(m->name, sizeof(m->name), "%s", name ? name : "");
}

void jce_state_set_entity_enabled(uint32_t id, bool enabled)
{
    HistoryEditScope edit_scope;

    if (!s.scene || id == 0) return;
    JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)id);
    if (!m) return;
    m->enabled = enabled;

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

    /* Cycle detection: ensure new_parent is not a descendant of id. */
    uint32_t check = new_parent;
    while (check != 0) {
        if (check == id) return;
        check = (uint32_t)jce_scene_get_parent(s.scene, (JceEntity)check);
    }

    /* Preserve the child's WORLD pose across the reparent: capture it before
       changing the parent, then back-solve a new LOCAL transform relative to
       the new parent so the object does not visually jump. */
    jce_mat4 child_world = jce_scene_get_world_matrix(s.scene, (JceEntity)id);

    jce_scene_set_parent(s.scene, (JceEntity)id,
                         new_parent != 0 ? (JceEntity)new_parent : JCE_ENTITY_INVALID);

    JceTransform *t = jce_scene_get_transform(s.scene, (JceEntity)id);
    if (t) {
        jce_mat4 parent_world = (new_parent != 0)
            ? jce_scene_get_world_matrix(s.scene, (JceEntity)new_parent)
            : jce_m4_identity();
        jce_mat4 inv   = jce_m4_inverse(&parent_world);
        jce_mat4 local = jce_m4_multiply(&inv, &child_world);
        jce_m4_decompose(&local, &t->position, &t->rotation, &t->scale);
    }
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
    g_entity_order.erase(it_e);

    auto it_r = std::find(g_entity_order.begin(), g_entity_order.end(), ref_id);
    if (it_r == g_entity_order.end()) {
        g_entity_order.push_back(entity_id);
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

void          jce_state_set_gizmo_mode(JceGizmoMode m)     { s.gizmo_mode = m; }
JceGizmoMode  jce_state_get_gizmo_mode(void)               { return s.gizmo_mode; }

void          jce_state_set_gizmo_space(JceGizmoSpace sp)  { s.gizmo_space = sp; }
JceGizmoSpace jce_state_get_gizmo_space(void)              { return s.gizmo_space; }

void          jce_state_set_gizmo_pivot(JceGizmoPivot p)   { s.gizmo_pivot = p; }
JceGizmoPivot jce_state_get_gizmo_pivot(void)              { return s.gizmo_pivot; }
void          jce_state_set_pivot_edit_mode(bool enabled)  { s.pivot_edit_mode = enabled; }
bool          jce_state_get_pivot_edit_mode(void)          { return s.pivot_edit_mode; }

/* Persist view_mode, show_grid and gizmo snap increments to
 * editor-config.json. */
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
}

void              jce_state_set_view_mode(JceSceneViewMode m)  { s.view_mode = m; persist_render_settings(); }
JceSceneViewMode  jce_state_get_view_mode(void)                { return s.view_mode; }

bool  jce_state_get_show_grid(void)          { return s.show_grid; }
void  jce_state_set_show_grid(bool show)     { s.show_grid = show; persist_render_settings(); }

float jce_state_get_gizmo_snap_translate(void) { return s_gizmo_snap_translate; }
float jce_state_get_gizmo_snap_rotate(void)    { return s_gizmo_snap_rotate; }
float jce_state_get_gizmo_snap_scale(void)     { return s_gizmo_snap_scale; }
bool  jce_state_get_gizmo_snap_enabled(void)   { return s_gizmo_snap_enabled; }
void  jce_state_set_gizmo_snap_enabled(bool v) { s_gizmo_snap_enabled = v; }

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

static bool s_show_physics_debug = false;
bool  jce_state_get_show_physics_debug(void)  { return s_show_physics_debug; }
void  jce_state_set_show_physics_debug(bool v){ s_show_physics_debug = v; }

static bool s_show_joint_gizmos = true;
bool  jce_state_get_show_joint_gizmos(void)   { return s_show_joint_gizmos; }
void  jce_state_set_show_joint_gizmos(bool v) { s_show_joint_gizmos = v; }

static bool s_show_cloth_gizmos = true;
bool  jce_state_get_show_cloth_gizmos(void)   { return s_show_cloth_gizmos; }
void  jce_state_set_show_cloth_gizmos(bool v) { s_show_cloth_gizmos = v; }

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
    | JCE_SHOW_FLAG_WORLD_AXIS;

uint32_t jce_state_get_show_flags(void)            { return s_show_flags; }
void     jce_state_set_show_flags(uint32_t flags)  { s_show_flags = flags; }
bool     jce_state_show_flag(JceShowFlag f)        { return (s_show_flags & (uint32_t)f) != 0; }
void     jce_state_set_show_flag(JceShowFlag f, bool on)
{
    if (on) s_show_flags |=  (uint32_t)f;
    else    s_show_flags &= ~(uint32_t)f;
}

bool  jce_state_get_2d_mode(void)            { return s.is_2d_mode; }
void  jce_state_set_2d_mode(bool is_2d)      { s.is_2d_mode = is_2d; }

bool  jce_state_get_live_preview(void)       { return s.live_preview; }
void  jce_state_set_live_preview(bool on)    { s.live_preview = on; }

void jce_state_set_scene(JceScene *scene) { s.scene = scene; }
JceScene *jce_state_get_scene(void) { return s.scene; }


bool jce_state_is_scene_modified(void) { return s.scene_modified; }
void jce_state_mark_scene_modified(void) { s.scene_modified = true; }
void jce_state_clear_scene_modified(void) { s.scene_modified = false; }
