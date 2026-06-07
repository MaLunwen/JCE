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

#include <jce/os/core/jce_str.h>
#include <jce/middleware/physics/jce_cloth.h>

/* Forward-declare only the one function we need from scene_render,
   avoiding a full include that creates a cpp-level circular dependency. */
extern "C" void jce_editor_scene_set_scene_dir(const char *dir);

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

void jce_editor_state_init(void)
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
            if (vm >= JCE_VIEW_SHADED && vm <= JCE_VIEW_TEXTURED)
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

    {
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
        jce_editor_scene_ensure_rendering_settings(s.scene);
    }

    s.initialized = true;
    LOG_INFO(LOG_TAG, "editor state initialized (%d demo entities)",
             (int)g_entity_order.size());
}

bool jce_state_new_default_scene(void)
{
    /* If Play is running it holds the current JceScene; recreating /
     * clearing it here would be a use-after-free.  Tear the runtime down
     * first. */
    stop_play_before_scene_swap();

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
    if (jce_scene_has_transform(s.scene, src))
        jce_scene_set_transform(s.scene, dst, jce_scene_get_transform(s.scene, src));
    if (jce_scene_has_mesh_renderer(s.scene, src))
        jce_scene_set_mesh_renderer(s.scene, dst, jce_scene_get_mesh_renderer(s.scene, src));
    if (jce_scene_has_compound_collider(s.scene, src))
        jce_scene_set_compound_collider(s.scene, dst, jce_scene_get_compound_collider(s.scene, src));
    if (jce_scene_has_camera(s.scene, src))
        jce_scene_set_camera(s.scene, dst, jce_scene_get_camera(s.scene, src));
    if (jce_scene_has_dir_light(s.scene, src))
        jce_scene_set_dir_light(s.scene, dst, jce_scene_get_dir_light(s.scene, src));
    if (jce_scene_has_point_light(s.scene, src))
        jce_scene_set_point_light(s.scene, dst, jce_scene_get_point_light(s.scene, src));
    if (jce_scene_has_spot_light(s.scene, src))
        jce_scene_set_spot_light(s.scene, dst, jce_scene_get_spot_light(s.scene, src));
    if (jce_scene_has_skybox(s.scene, src))
        jce_scene_set_skybox(s.scene, dst, jce_scene_get_skybox(s.scene, src));
    if (jce_scene_has_sprite_renderer(s.scene, src))
        jce_scene_set_sprite_renderer(s.scene, dst, jce_scene_get_sprite_renderer(s.scene, src));
    if (jce_scene_has_sprite_animator(s.scene, src))
        jce_scene_set_sprite_animator(s.scene, dst, jce_scene_get_sprite_animator(s.scene, src));
    if (jce_scene_has_animator(s.scene, src))
        jce_scene_set_animator(s.scene, dst, jce_scene_get_animator(s.scene, src));
    if (jce_scene_has_skeletal_animator(s.scene, src))
        jce_scene_set_skeletal_animator(s.scene, dst, jce_scene_get_skeletal_animator(s.scene, src));
    if (jce_scene_has_constraint(s.scene, src))
        jce_scene_set_constraint(s.scene, dst, jce_scene_get_constraint(s.scene, src));
    if (jce_scene_has_rigidbody(s.scene, src))
        jce_scene_set_rigidbody(s.scene, dst, jce_scene_get_rigidbody(s.scene, src));
    if (jce_scene_has_box_collider(s.scene, src))
        jce_scene_set_box_collider(s.scene, dst, jce_scene_get_box_collider(s.scene, src));
    if (jce_scene_has_sphere_collider(s.scene, src))
        jce_scene_set_sphere_collider(s.scene, dst, jce_scene_get_sphere_collider(s.scene, src));
    if (jce_scene_has_character_controller(s.scene, src))
        jce_scene_set_character_controller(s.scene, dst, jce_scene_get_character_controller(s.scene, src));
    if (jce_scene_has_audio_source(s.scene, src))
        jce_scene_set_audio_source(s.scene, dst, jce_scene_get_audio_source(s.scene, src));
    if (jce_scene_has_script(s.scene, src))
        jce_scene_set_script(s.scene, dst, jce_scene_get_script(s.scene, src));
    /* New this-sprint components carrying engine-owned runtime handles.
     * VideoPlayer's flecs copy hook duplicates the authoring fields only and
     * clears the duplicate's decoder/texture, so a plain set is safe.
     * ParticleEmitter has NO copy hook (the scene uses mark-and-sweep), so
     * ecs_set_ptr memcpys `loaded`/`emitter_handle_idx` verbatim — leaving the
     * copy aliasing the source's live emitter (jce_scene_particles.c skips the
     * rebuild while loaded && asset_epoch matches).  Reset the duplicate's
     * runtime bookkeeping so it builds its OWN emitter on first tick. */
    if (jce_scene_has_particle_emitter(s.scene, src)) {
        jce_scene_set_particle_emitter(s.scene, dst, jce_scene_get_particle_emitter(s.scene, src));
        if (JceParticleEmitterComponent *pe = jce_scene_get_particle_emitter(s.scene, dst)) {
            pe->loaded             = false;
            pe->emitter_handle_idx = UINT32_MAX;
            pe->asset_epoch        = 0;
        }
    }
    if (jce_scene_has_video_player(s.scene, src))
        jce_scene_set_video_player(s.scene, dst, jce_scene_get_video_player(s.scene, src));
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

void jce_state_add_component(uint32_t entity_id, uint64_t comp_flag)
{
    HistoryEditScope edit_scope;

    if (!s.scene || entity_id == 0) return;
    JceEntity e = (JceEntity)entity_id;

    switch (comp_flag) {
    case JCE_COMP_FLAG_TRANSFORM: {
        if (jce_scene_has_transform(s.scene, e)) return;
        JceTransform t;
        t.position = jce_v3(0.0f, 0.0f, 0.0f);
        t.rotation = jce_q_identity();
        t.scale    = jce_v3(1.0f, 1.0f, 1.0f);
        jce_scene_set_transform(s.scene, e, &t);
        break;
    }
    case JCE_COMP_FLAG_MESH_RENDERER: {
        if (jce_scene_has_mesh_renderer(s.scene, e)) return;
        JceMeshRenderer mr;
        memset(&mr, 0, sizeof(mr));
        mr.visible        = true;
        mr.base_color[0]  = 1.0f;
        mr.base_color[1]  = 1.0f;
        mr.base_color[2]  = 1.0f;
        mr.base_color[3]  = 1.0f;
        mr.metallic       = 0.0f;
        mr.roughness      = 0.5f;
        mr.normal_scale   = 1.0f;
        mr.ao_strength    = 1.0f;
        mr.alpha_cutoff   = 0.5f;
        jce_scene_set_mesh_renderer(s.scene, e, &mr);
        break;
    }
    case JCE_COMP_FLAG_CAMERA: {
        if (jce_scene_has_camera(s.scene, e)) return;
        JceCameraComponent c;
        memset(&c, 0, sizeof(c));
        c.fov_deg    = 60.0f;
        c.near_plane = 0.1f;
        c.far_plane  = 1000.0f;
        c.is_primary = false;
        c.ortho      = false;
        jce_scene_set_camera(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_DIR_LIGHT: {
        if (jce_scene_has_dir_light(s.scene, e)) return;
        JceDirectionalLight l;
        l.direction    = jce_v3(0.0f, -1.0f, 0.0f);
        l.color        = jce_v3(1.0f, 1.0f, 1.0f);
        l.intensity    = 1.0f;
        l.casts_shadow = true;
        jce_scene_set_dir_light(s.scene, e, &l);
        break;
    }
    case JCE_COMP_FLAG_POINT_LIGHT: {
        if (jce_scene_has_point_light(s.scene, e)) return;
        JcePointLight l;
        l.position  = jce_v3(0.0f, 0.0f, 0.0f);
        l.color     = jce_v3(1.0f, 1.0f, 1.0f);
        l.intensity = 1.0f;
        l.radius    = 10.0f;
        jce_scene_set_point_light(s.scene, e, &l);
        break;
    }
    case JCE_COMP_FLAG_SPOT_LIGHT: {
        if (jce_scene_has_spot_light(s.scene, e)) return;
        JceSpotLight l;
        l.position       = jce_v3(0.0f, 0.0f, 0.0f);
        l.direction      = jce_v3(0.0f, -1.0f, 0.0f);
        l.color          = jce_v3(1.0f, 1.0f, 1.0f);
        l.intensity      = 1.0f;
        l.radius         = 10.0f;
        l.inner_cone_cos = 0.95f;
        l.outer_cone_cos = 0.85f;
        jce_scene_set_spot_light(s.scene, e, &l);
        break;
    }
    case JCE_COMP_FLAG_SKYBOX: {
        if (jce_scene_has_skybox(s.scene, e)) return;
        JceSkyboxComponent c;
        memset(&c, 0, sizeof(c));
        c.exposure   = 1.0f;
        c.use_as_ibl = true;
        jce_scene_set_skybox(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_SPRITE_RENDERER: {
        if (jce_scene_has_sprite_renderer(s.scene, e)) return;
        JceSpriteRendererComponent c;
        memset(&c, 0, sizeof(c));
        c.color[0] = 1.0f; c.color[1] = 1.0f;
        c.color[2] = 1.0f; c.color[3] = 1.0f;
        jce_scene_set_sprite_renderer(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_SPRITE_ANIMATOR: {
        if (jce_scene_has_sprite_animator(s.scene, e)) return;
        JceSpriteAnimatorComponent c;
        memset(&c, 0, sizeof(c));
        c.frame_width  = 64;
        c.frame_height = 64;
        c.speed        = 1.0f;
        c.loop         = true;
        jce_scene_set_sprite_animator(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_ANIMATOR: {
        if (jce_scene_has_animator(s.scene, e)) return;
        JceAnimatorComponent c;
        memset(&c, 0, sizeof(c));
        c.speed = 1.0f;
        c.loop  = true;
        jce_scene_set_animator(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_SKELETAL_ANIMATOR: {
        if (jce_scene_has_skeletal_animator(s.scene, e)) return;
        JceSkeletalAnimatorComponent c;
        memset(&c, 0, sizeof(c));
        c.speed = 1.0f;
        c.loop  = true;
        jce_scene_set_skeletal_animator(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_CONSTRAINT: {
        if (jce_scene_has_constraint(s.scene, e)) return;
        JceConstraintComponent c;
        memset(&c, 0, sizeof(c));
        jce_scene_set_constraint(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_RIGIDBODY: {
        if (jce_scene_has_rigidbody(s.scene, e)) return;
        JceRigidBodyComponent c;
        memset(&c, 0, sizeof(c));
        c.mass          = 1.0f;
        c.friction      = 0.5f;
        c.restitution   = 0.0f;
        c.use_gravity   = true;
        c.gravity_scale = 1.0f;
        jce_scene_set_rigidbody(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_BOX_COLLIDER: {
        if (jce_scene_has_box_collider(s.scene, e)) return;
        JceBoxColliderComponent c;
        memset(&c, 0, sizeof(c));
        c.size[0] = 1.0f; c.size[1] = 1.0f; c.size[2] = 1.0f;
        jce_scene_set_box_collider(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_SPHERE_COLLIDER: {
        if (jce_scene_has_sphere_collider(s.scene, e)) return;
        JceSphereColliderComponent c;
        memset(&c, 0, sizeof(c));
        c.radius = 0.5f;
        jce_scene_set_sphere_collider(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_CHARACTER_CONTROLLER: {
        if (jce_scene_has_character_controller(s.scene, e)) return;
        JceCharacterControllerComponent c;
        c.height      = 2.0f;
        c.radius      = 0.3f;
        c.step_offset = 0.35f;
        c.slope_limit = 45.0f;
        jce_scene_set_character_controller(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_AUDIO_SOURCE: {
        if (jce_scene_has_audio_source(s.scene, e)) return;
        JceAudioSourceComponent c;
        memset(&c, 0, sizeof(c));
        c.volume = 1.0f;
        c.pitch  = 1.0f;
        jce_scene_set_audio_source(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_SCRIPT: {
        if (jce_scene_has_script(s.scene, e)) return;
        JceScriptComponent c;
        memset(&c, 0, sizeof(c));
        jce_scene_set_script(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_TERRAIN: {
        if (jce_scene_has_terrain(s.scene, e)) return;
        JceTerrainComponent c;
        memset(&c, 0, sizeof(c));
        c.tint[0] = c.tint[1] = c.tint[2] = 1.0f;
        c.visible = true;
        c.tile_scale = 10.0f;
        c.splat_enabled = true;
        jce_scene_set_terrain(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_RIGIDBODY_2D: {
        if (jce_scene_has_rigidbody2d(s.scene, e)) return;
        JceRigidBody2DComponent c;
        memset(&c, 0, sizeof(c));
        c.mass = 1.0f;
        c.friction = 0.5f;
        c.restitution = 0.0f;
        c.fixed_rotation = false;
        jce_scene_set_rigidbody2d(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_PARTICLE_EMITTER: {
        if (jce_scene_has_particle_emitter(s.scene, e)) return;
        JceParticleEmitterComponent c;
        memset(&c, 0, sizeof(c));
        c.emit_rate    = 10.0f;
        c.lifetime_min = 1.0f;
        c.lifetime_max = 2.0f;
        jce_scene_set_particle_emitter(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_BEHAVIOR_TREE: {
        if (jce_scene_has_behavior_tree(s.scene, e)) return;
        JceBehaviorTree c;
        memset(&c, 0, sizeof(c));
        c.active = true;
        jce_scene_set_behavior_tree(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_LOD_GROUP: {
        if (jce_scene_has_lod_group(s.scene, e)) return;
        JceLodGroupComponent c;
        memset(&c, 0, sizeof(c));
        c.level_count = 3;
        c.distances[0] = 15.0f;
        c.distances[1] = 50.0f;
        c.distances[2] = 150.0f;
        c.hysteresis = 0.05f;
        c.cull_when_too_far = true;
        jce_scene_set_lod_group(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_VIRTUAL_CAMERA: {
        if (jce_scene_has_virtual_camera(s.scene, e)) return;
        JceVirtualCameraComponent c;
        memset(&c, 0, sizeof(c));
        snprintf(c.vcam_name, sizeof(c.vcam_name), "VCam");
        c.priority   = 10;
        c.active     = true;
        c.track_mode = 0;
        c.fov_deg    = 60.0f;
        c.damping    = 0.5f;
        jce_scene_set_virtual_camera(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_TRIGGER_VOLUME: {
        if (jce_scene_has_trigger_volume(s.scene, e)) return;
        JceTriggerVolumeComponent c;
        memset(&c, 0, sizeof(c));
        c.shape = 0; /* AABB */
        c.half_extents[0] = c.half_extents[1] = c.half_extents[2] = 0.5f;
        c.axis_x[0] = 1.0f; c.axis_y[1] = 1.0f; c.axis_z[2] = 1.0f;
        c.enabled = true;
        c.fire_stay = false;
        jce_scene_set_trigger_volume(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_CAPSULE_COLLIDER: {
        if (jce_scene_has_capsule_collider(s.scene, e)) return;
        JceCapsuleColliderComponent c;
        memset(&c, 0, sizeof(c));
        c.radius = 0.5f;
        c.height = 2.0f;
        c.axis   = 1; /* Y */
        jce_scene_set_capsule_collider(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_MESH_COLLIDER: {
        if (jce_scene_has_mesh_collider(s.scene, e)) return;
        JceMeshColliderComponent c;
        memset(&c, 0, sizeof(c));
        c.friction    = 0.5f;
        c.restitution = 0.0f;
        jce_scene_set_mesh_collider(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_COLLIDER_2D: {
        if (jce_scene_has_collider2d(s.scene, e)) return;
        JceCollider2DComponent c;
        memset(&c, 0, sizeof(c));
        c.shape = 0; /* Box */
        c.size[0] = c.size[1] = 1.0f;
        c.radius  = 0.5f;
        c.friction    = 0.4f;
        c.restitution = 0.0f;
        jce_scene_set_collider2d(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_TRAIL_RENDERER: {
        if (jce_scene_has_trail_renderer(s.scene, e)) return;
        JceTrailRendererComponent c;
        memset(&c, 0, sizeof(c));
        c.time = 1.0f;
        c.min_vertex_distance = 0.1f;
        c.width_start = 0.1f;
        c.width_end   = 0.0f;
        c.color_start[0] = c.color_start[1] = c.color_start[2] = c.color_start[3] = 1.0f;
        c.color_end[0]   = c.color_end[1]   = c.color_end[2]   = 1.0f;
        c.color_end[3]   = 0.0f;
        c.emitting = true;
        jce_scene_set_trail_renderer(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_LINE_RENDERER: {
        if (jce_scene_has_line_renderer(s.scene, e)) return;
        JceLineRendererComponent c;
        memset(&c, 0, sizeof(c));
        c.position_count = 2;
        c.positions[1][0] = 1.0f; /* default 2-point line along +X */
        c.width_start = 0.1f;
        c.width_end   = 0.1f;
        c.color_start[0] = c.color_start[1] = c.color_start[2] = c.color_start[3] = 1.0f;
        c.color_end[0]   = c.color_end[1]   = c.color_end[2]   = c.color_end[3]   = 1.0f;
        c.use_world_space = true;
        jce_scene_set_line_renderer(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_REFLECTION_PROBE: {
        if (jce_scene_has_reflection_probe(s.scene, e)) return;
        JceReflectionProbeComponent c;
        memset(&c, 0, sizeof(c));
        c.mode = 0; /* Baked */
        c.resolution = 128;
        c.intensity = 1.0f;
        c.box_size[0] = c.box_size[1] = c.box_size[2] = 10.0f;
        c.near_clip = 0.3f;
        c.far_clip  = 1000.0f;
        c.box_projection = true;
        c.hdr = true;
        jce_scene_set_reflection_probe(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_DECAL: {
        if (jce_scene_has_decal(s.scene, e)) return;
        JceDecalComponent c;
        memset(&c, 0, sizeof(c));
        c.size[0] = c.size[1] = c.size[2] = 1.0f;
        c.color[0] = c.color[1] = c.color[2] = c.color[3] = 1.0f;
        c.opacity = 1.0f;
        c.draw_distance = 1000.0f;
        c.fade_factor = 1.0f;
        c.layer_mask = -1;
        jce_scene_set_decal(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_LIGHT_PROBE_GROUP: {
        if (jce_scene_has_light_probe_group(s.scene, e)) return;
        JceLightProbeGroupComponent c;
        memset(&c, 0, sizeof(c));
        /* Default: 8 corners of a unit cube. */
        c.probe_count = 8;
        for (int i = 0; i < 8; ++i) {
            c.positions[i][0] = (i & 1) ? 1.0f : -1.0f;
            c.positions[i][1] = (i & 2) ? 1.0f : -1.0f;
            c.positions[i][2] = (i & 4) ? 1.0f : -1.0f;
        }
        jce_scene_set_light_probe_group(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_AUDIO_LISTENER: {
        if (jce_scene_has_audio_listener(s.scene, e)) return;
        JceAudioListenerComponent c;
        memset(&c, 0, sizeof(c));
        c.volume = 1.0f;
        c.spatialize = true;
        c.doppler_factor = 1.0f;
        jce_scene_set_audio_listener(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_AUDIO_REVERB_ZONE: {
        if (jce_scene_has_audio_reverb_zone(s.scene, e)) return;
        JceAudioReverbZoneComponent c;
        memset(&c, 0, sizeof(c));
        c.preset = JCE_REVERB_ZONE_PRESET_GENERIC;
        c.min_distance = 10.0f;
        c.max_distance = 15.0f;
        c.room = -1000.0f;
        c.room_hf = -100.0f;
        c.decay_time = 1.49f;
        c.decay_hf_ratio = 0.83f;
        c.reflections = -2602.0f;
        c.reflections_delay = 0.007f;
        c.reverb = 200.0f;
        c.reverb_delay = 0.011f;
        c.hf_reference = 5000.0f;
        c.diffusion = 100.0f;
        c.density = 100.0f;
        jce_scene_set_audio_reverb_zone(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_AUDIO_OCCLUSION: {
        if (jce_scene_has_audio_occlusion(s.scene, e)) return;
        JceAudioOcclusionComponent c;
        memset(&c, 0, sizeof(c));
        c.radius = 5.0f;
        c.attenuation_db = -12.0f;
        c.lowpass_cutoff_hz = 1000.0f;
        c.layer_mask = -1;
        c.affects_reverb = true;
        jce_scene_set_audio_occlusion(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_SPAWN_MANAGER: {
        if (jce_scene_has_spawn_manager(s.scene, e)) return;
        JceSpawnManagerComponent c;
        memset(&c, 0, sizeof(c));
        c.enabled = 1;
        c.max_peds = 32;
        c.max_vehicles = 16;
        c.min_spawn_radius = 30.0f;
        c.max_spawn_radius = 120.0f;
        c.despawn_pad = 30.0f;
        c.spawn_interval = 0.5f;
        jce_scene_set_spawn_manager(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_WEAPON: {
        if (jce_scene_has_weapon(s.scene, e)) return;
        JceWeaponComponent c;
        memset(&c, 0, sizeof(c));
        snprintf(c.name, sizeof(c.name), "%s", "Weapon");
        c.kind = JCE_WEAPON_COMP_HITSCAN;
        c.damage = 10.0f;
        c.range = 100.0f;
        c.rpm = 600.0f;
        c.clip_size = 30;
        c.reserve_max = 120;
        c.reload_seconds = 2.0f;
        c.spread_deg = 0.5f;
        c.recoil_per_shot = 0.5f;
        c.recoil_recovery = 8.0f;
        c.pellets = 1;
        c.projectile_speed = 200.0f;
        c.full_auto = false;
        jce_scene_set_weapon(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_SAVE_POINT: {
        if (jce_scene_has_save_point(s.scene, e)) return;
        JceSavePointComponent c;
        memset(&c, 0, sizeof(c));
        snprintf(c.save_id,      sizeof(c.save_id),      "%s", "save_point");
        snprintf(c.display_name, sizeof(c.display_name), "%s", "Save Point");
        c.kind = JCE_SAVE_POINT_MANUAL;
        c.radius = 1.5f;
        c.slot = -1;
        c.one_shot = false;
        c.require_interact = true;
        jce_scene_set_save_point(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_WHEEL_COLLIDER: {
        if (jce_scene_has_wheel_collider(s.scene, e)) return;
        JceWheelColliderComponent c;
        memset(&c, 0, sizeof(c));
        c.radius = 0.5f;
        c.suspension_distance = 0.3f;
        c.suspension_spring = 35000.0f;
        c.suspension_damper = 4500.0f;
        c.suspension_target_pos = 0.5f;
        c.mass = 20.0f;
        c.forward_friction = 1.0f;
        c.sideways_friction = 1.0f;
        jce_scene_set_wheel_collider(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_CONSTANT_FORCE: {
        if (jce_scene_has_constant_force(s.scene, e)) return;
        JceConstantForceComponent c;
        memset(&c, 0, sizeof(c));
        c.enabled = true;
        jce_scene_set_constant_force(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_CONFIGURABLE_JOINT: {
        if (jce_scene_has_configurable_joint(s.scene, e)) return;
        JceConfigurableJointComponent c;
        memset(&c, 0, sizeof(c));
        c.x_motion = c.y_motion = c.z_motion = JCE_CFG_JOINT_LOCKED;
        c.x_rotation = c.y_rotation = c.z_rotation = JCE_CFG_JOINT_FREE;
        c.linear_limit = 0.0f;
        c.angular_x_limit_deg = 45.0f;
        c.angular_y_limit_deg = 45.0f;
        c.angular_z_limit_deg = 45.0f;
        c.break_force = 3.4e38f;
        c.break_torque = 3.4e38f;
        c.enable_collision = false;
        jce_scene_set_configurable_joint(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_CLOTH: {
        if (jce_scene_has_cloth(s.scene, e)) return;
        JceClothComponent c;
        memset(&c, 0, sizeof(c));
        /* 1x1 m horizontal patch as a sensible default. */
        c.corner_00 = jce_v3(0.0f, 0.0f, 0.0f);
        c.corner_10 = jce_v3(1.0f, 0.0f, 0.0f);
        c.corner_01 = jce_v3(0.0f, 0.0f, 1.0f);
        c.corner_11 = jce_v3(1.0f, 0.0f, 1.0f);
        c.res_u = 8;
        c.res_v = 8;
        c.mass_total = 1.0f;
        c.stiffness_linear  = 0.5f;
        c.stiffness_angular = 0.5f;
        c.damping    = 0.02f;
        c.iterations = 4;
        c.self_collision = false;
        c.wind_enabled   = false;
        c.wind_velocity  = jce_v3(0.0f, 0.0f, 0.0f);
        c.pinned_count   = 0;
        c.handle = 0;
        c.dirty  = true;
        jce_scene_set_cloth(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_NET_TRANSFORM: {
        if (jce_scene_has_net_transform(s.scene, e)) return;
        JceNetTransformComponent c; memset(&c, 0, sizeof c);
        c.sync_rate_hz   = 20;
        c.interp_ms      = 100;
        c.tolerance      = 0.5f;
        c.authority_mode = 0;
        jce_scene_set_net_transform(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_NET_ANIMATOR: {
        if (jce_scene_has_net_animator(s.scene, e)) return;
        JceNetAnimatorComponent c; memset(&c, 0, sizeof c);
        c.sync_rate_hz   = 20;
        c.interp_ms      = 100;
        c.authority_mode = 0;
        jce_scene_set_net_animator(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_NET_RIGIDBODY: {
        if (jce_scene_has_net_rigidbody(s.scene, e)) return;
        JceNetRigidbodyComponent c; memset(&c, 0, sizeof c);
        c.sync_rate_hz   = 20;
        c.interp_ms      = 100;
        c.tolerance      = 0.5f;
        c.authority_mode = 0;
        jce_scene_set_net_rigidbody(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_VFX_GRAPH: {
        if (jce_scene_has_vfx_graph(s.scene, e)) return;
        JceVfxGraphComponent c; memset(&c, 0, sizeof c);
        c.play_on_awake   = true;
        c.loop            = true;
        c.rate_multiplier = 1.0f;
        c.intensity       = 1.0f;
        jce_scene_set_vfx_graph(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_TILEMAP: {
        if (jce_scene_has_tilemap(s.scene, e)) return;
        JceTilemapComponent c; memset(&c, 0, sizeof c);
        c.cell_size_px = 16;
        c.sort_order   = 0;
        c.orientation  = 0;
        c.visible      = true;
        c.color[0] = c.color[1] = c.color[2] = c.color[3] = 1.0f;
        jce_scene_set_tilemap(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_TILEMAP_COLLIDER_2D: {
        if (jce_scene_has_tilemap_collider2d(s.scene, e)) return;
        JceTilemapCollider2DComponent c; memset(&c, 0, sizeof c);
        c.friction_x100   = 40;
        c.bounciness_x100 = 0;
        jce_scene_set_tilemap_collider2d(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_AVATAR: {
        if (jce_scene_has_avatar(s.scene, e)) return;
        JceAvatarComponent c; memset(&c, 0, sizeof c);
        c.human_rig = true;
        jce_scene_set_avatar(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_JOINT_2D: {
        if (jce_scene_has_joint2d(s.scene, e)) return;
        JceJoint2DComponent c;
        memset(&c, 0, sizeof(c));
        c.kind = JCE_JOINT_2D_DISTANCE;
        c.distance = 1.0f;
        c.frequency = 5.0f;
        c.damping_ratio = 0.7f;
        c.motor_speed_deg_s = 90.0f;
        c.motor_max_torque = 10000.0f;
        c.lower_angle_deg = -90.0f;
        c.upper_angle_deg =  90.0f;
        c.break_force = 3.4e38f;
        c.break_torque = 3.4e38f;
        c.auto_configure_distance = true;
        jce_scene_set_joint2d(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_BILLBOARD_RENDERER: {
        if (jce_scene_has_billboard_renderer(s.scene, e)) return;
        JceBillboardRendererComponent c;
        memset(&c, 0, sizeof(c));
        c.mode = JCE_BILLBOARD_FULL;
        c.size[0] = 1.0f; c.size[1] = 1.0f;
        c.color[0] = c.color[1] = c.color[2] = c.color[3] = 1.0f;
        c.visible = true;
        jce_scene_set_billboard_renderer(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_CANVAS: {
        if (jce_scene_has_canvas(s.scene, e)) return;
        JceCanvasComponent c;
        memset(&c, 0, sizeof(c));
        c.render_mode = JCE_CANVAS_OVERLAY;
        c.sort_order = 0;
        c.reference_resolution[0] = 1920.0f;
        c.reference_resolution[1] = 1080.0f;
        c.scale_factor = 1.0f;
        c.pixel_perfect = false;
        jce_scene_set_canvas(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_CANVAS_GROUP: {
        if (jce_scene_has_canvas_group(s.scene, e)) return;
        JceCanvasGroupComponent c;
        memset(&c, 0, sizeof(c));
        c.alpha = 1.0f;
        c.interactable = true;
        c.blocks_raycasts = true;
        c.ignore_parent_groups = false;
        jce_scene_set_canvas_group(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_LAYOUT_GROUP: {
        if (jce_scene_has_layout_group(s.scene, e)) return;
        JceLayoutGroupComponent c;
        memset(&c, 0, sizeof(c));
        c.layout_kind = JCE_LAYOUT_VERTICAL;
        c.spacing[0] = c.spacing[1] = 4.0f;
        c.cell_size[0] = c.cell_size[1] = 64.0f;
        c.child_alignment = 0;
        c.control_child_size_w = true;
        c.control_child_size_h = false;
        jce_scene_set_layout_group(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_UI_IMAGE: {
        if (jce_scene_has_ui_image(s.scene, e)) return;
        JceUIImageComponent c;
        memset(&c, 0, sizeof(c));
        c.image_type = JCE_UI_IMAGE_SIMPLE;
        c.color[0] = c.color[1] = c.color[2] = c.color[3] = 1.0f;
        c.fill_amount = 1.0f;
        c.preserve_aspect = false;
        c.raycast_target = true;
        jce_scene_set_ui_image(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_UI_TEXT: {
        if (jce_scene_has_ui_text(s.scene, e)) return;
        JceUITextComponent c;
        memset(&c, 0, sizeof(c));
        snprintf(c.text, sizeof(c.text), "%s", "New Text");
        c.font_size = 14.0f;
        c.alignment = JCE_UI_TEXT_ALIGN_LEFT;
        c.color[0] = c.color[1] = c.color[2] = c.color[3] = 1.0f;
        c.line_spacing = 1.0f;
        c.min_size = 10;
        c.max_size = 40;
        jce_scene_set_ui_text(s.scene, e, &c);
        break;
    }
    case JCE_COMP_FLAG_UI_BUTTON: {
        if (jce_scene_has_ui_button(s.scene, e)) return;
        JceUIButtonComponent c;
        memset(&c, 0, sizeof(c));
        c.interactable = true;
        c.normal_color[0] = c.normal_color[1] = c.normal_color[2] = c.normal_color[3] = 1.0f;
        c.highlighted_color[0] = 0.96f; c.highlighted_color[1] = 0.96f; c.highlighted_color[2] = 0.96f; c.highlighted_color[3] = 1.0f;
        c.pressed_color[0] = 0.78f; c.pressed_color[1] = 0.78f; c.pressed_color[2] = 0.78f; c.pressed_color[3] = 1.0f;
        c.disabled_color[0] = 0.78f; c.disabled_color[1] = 0.78f; c.disabled_color[2] = 0.78f; c.disabled_color[3] = 0.50f;
        c.fade_duration = 0.1f;
        jce_scene_set_ui_button(s.scene, e, &c);
        break;
    }
    default:
        return;
    }

    if (!s_suppress_add_component_log) {
        LOG_INFO(LOG_TAG, "add component %s to entity %u",
                 jce_comp_flag_display_name(comp_flag), entity_id);
    }
}

void jce_state_remove_component(uint32_t entity_id, uint64_t comp_flag)
{
    HistoryEditScope edit_scope;

    if (!s.scene || entity_id == 0) return;
    JceEntity e = (JceEntity)entity_id;

    switch (comp_flag) {
    case JCE_COMP_FLAG_TRANSFORM:            jce_scene_remove_transform(s.scene, e); break;
    case JCE_COMP_FLAG_MESH_RENDERER:        jce_scene_remove_mesh_renderer(s.scene, e); break;
    case JCE_COMP_FLAG_CAMERA:               jce_scene_remove_camera(s.scene, e); break;
    case JCE_COMP_FLAG_DIR_LIGHT:            jce_scene_remove_dir_light(s.scene, e); break;
    case JCE_COMP_FLAG_POINT_LIGHT:          jce_scene_remove_point_light(s.scene, e); break;
    case JCE_COMP_FLAG_SPOT_LIGHT:           jce_scene_remove_spot_light(s.scene, e); break;
    case JCE_COMP_FLAG_SKYBOX:               jce_scene_remove_skybox(s.scene, e); break;
    case JCE_COMP_FLAG_SPRITE_RENDERER:      jce_scene_remove_sprite_renderer(s.scene, e); break;
    case JCE_COMP_FLAG_SPRITE_ANIMATOR:      jce_scene_remove_sprite_animator(s.scene, e); break;
    case JCE_COMP_FLAG_ANIMATOR:             jce_scene_remove_animator(s.scene, e); break;
    case JCE_COMP_FLAG_SKELETAL_ANIMATOR:    jce_scene_remove_skeletal_animator(s.scene, e); break;
    case JCE_COMP_FLAG_CONSTRAINT:           jce_scene_remove_constraint(s.scene, e); break;
    case JCE_COMP_FLAG_RIGIDBODY:            jce_scene_remove_rigidbody(s.scene, e); break;
    case JCE_COMP_FLAG_BOX_COLLIDER:         jce_scene_remove_box_collider(s.scene, e); break;
    case JCE_COMP_FLAG_SPHERE_COLLIDER:      jce_scene_remove_sphere_collider(s.scene, e); break;
    case JCE_COMP_FLAG_CHARACTER_CONTROLLER: jce_scene_remove_character_controller(s.scene, e); break;
    case JCE_COMP_FLAG_AUDIO_SOURCE:         jce_scene_remove_audio_source(s.scene, e); break;
    case JCE_COMP_FLAG_SCRIPT:               jce_scene_remove_script(s.scene, e); break;
    case JCE_COMP_FLAG_TERRAIN:              jce_scene_remove_terrain(s.scene, e); break;
    case JCE_COMP_FLAG_RIGIDBODY_2D:         jce_scene_remove_rigidbody2d(s.scene, e); break;
    case JCE_COMP_FLAG_PARTICLE_EMITTER:     jce_scene_remove_particle_emitter(s.scene, e); break;
    case JCE_COMP_FLAG_BEHAVIOR_TREE:        jce_scene_remove_behavior_tree(s.scene, e); break;
    case JCE_COMP_FLAG_LOD_GROUP:            jce_scene_remove_lod_group(s.scene, e); break;
    case JCE_COMP_FLAG_VIRTUAL_CAMERA:       jce_scene_remove_virtual_camera(s.scene, e); break;
    case JCE_COMP_FLAG_TRIGGER_VOLUME:       jce_scene_remove_trigger_volume(s.scene, e); break;
    case JCE_COMP_FLAG_CAPSULE_COLLIDER:     jce_scene_remove_capsule_collider(s.scene, e); break;
    case JCE_COMP_FLAG_MESH_COLLIDER:        jce_scene_remove_mesh_collider(s.scene, e); break;
    case JCE_COMP_FLAG_COLLIDER_2D:          jce_scene_remove_collider2d(s.scene, e); break;
    case JCE_COMP_FLAG_TRAIL_RENDERER:       jce_scene_remove_trail_renderer(s.scene, e); break;
    case JCE_COMP_FLAG_LINE_RENDERER:        jce_scene_remove_line_renderer(s.scene, e); break;
    case JCE_COMP_FLAG_REFLECTION_PROBE:     jce_scene_remove_reflection_probe(s.scene, e); break;
    case JCE_COMP_FLAG_DECAL:                jce_scene_remove_decal(s.scene, e); break;
    case JCE_COMP_FLAG_LIGHT_PROBE_GROUP:    jce_scene_remove_light_probe_group(s.scene, e); break;
    case JCE_COMP_FLAG_AUDIO_LISTENER:       jce_scene_remove_audio_listener(s.scene, e); break;
    case JCE_COMP_FLAG_AUDIO_REVERB_ZONE:    jce_scene_remove_audio_reverb_zone(s.scene, e); break;
    case JCE_COMP_FLAG_AUDIO_OCCLUSION:      jce_scene_remove_audio_occlusion(s.scene, e); break;
    case JCE_COMP_FLAG_SPAWN_MANAGER:        jce_scene_remove_spawn_manager(s.scene, e); break;
    case JCE_COMP_FLAG_WEAPON:               jce_scene_remove_weapon(s.scene, e); break;
    case JCE_COMP_FLAG_SAVE_POINT:           jce_scene_remove_save_point(s.scene, e); break;
    case JCE_COMP_FLAG_WHEEL_COLLIDER:       jce_scene_remove_wheel_collider(s.scene, e); break;
    case JCE_COMP_FLAG_CONSTANT_FORCE:       jce_scene_remove_constant_force(s.scene, e); break;
    case JCE_COMP_FLAG_CONFIGURABLE_JOINT:   jce_scene_remove_configurable_joint(s.scene, e); break;
    case JCE_COMP_FLAG_JOINT_2D:             jce_scene_remove_joint2d(s.scene, e); break;
    case JCE_COMP_FLAG_BILLBOARD_RENDERER:   jce_scene_remove_billboard_renderer(s.scene, e); break;
    case JCE_COMP_FLAG_CANVAS:               jce_scene_remove_canvas(s.scene, e); break;
    case JCE_COMP_FLAG_CANVAS_GROUP:         jce_scene_remove_canvas_group(s.scene, e); break;
    case JCE_COMP_FLAG_LAYOUT_GROUP:         jce_scene_remove_layout_group(s.scene, e); break;
    case JCE_COMP_FLAG_UI_IMAGE:             jce_scene_remove_ui_image(s.scene, e); break;
    case JCE_COMP_FLAG_UI_TEXT:              jce_scene_remove_ui_text(s.scene, e); break;
    case JCE_COMP_FLAG_UI_BUTTON:            jce_scene_remove_ui_button(s.scene, e); break;
    case JCE_COMP_FLAG_CLOTH: {
        /* Destroy the runtime cloth handle (if any) before dropping
         * the component so the cloth runtime doesn't leak. */
        JceClothComponent *cc = jce_scene_get_cloth(s.scene, e);
        if (cc && cc->handle != 0) {
            jce_cloth_destroy((JceClothHandle)cc->handle);
            cc->handle = 0;
        }
        jce_scene_remove_cloth(s.scene, e);
        break;
    }
    case JCE_COMP_FLAG_NET_TRANSFORM:        jce_scene_remove_net_transform(s.scene, e); break;
    case JCE_COMP_FLAG_NET_ANIMATOR:         jce_scene_remove_net_animator(s.scene, e); break;
    case JCE_COMP_FLAG_NET_RIGIDBODY:        jce_scene_remove_net_rigidbody(s.scene, e); break;
    case JCE_COMP_FLAG_VFX_GRAPH:            jce_scene_remove_vfx_graph(s.scene, e); break;
    case JCE_COMP_FLAG_TILEMAP:              jce_scene_remove_tilemap(s.scene, e); break;
    case JCE_COMP_FLAG_TILEMAP_COLLIDER_2D:  jce_scene_remove_tilemap_collider2d(s.scene, e); break;
    case JCE_COMP_FLAG_AVATAR:               jce_scene_remove_avatar(s.scene, e); break;
    default: return;
    }

    LOG_INFO(LOG_TAG, "remove component %s from entity %u",
             jce_comp_flag_display_name(comp_flag), entity_id);
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
    case JCE_COMP_FLAG_NET_TRANSFORM:        return "Network Transform";
    case JCE_COMP_FLAG_NET_ANIMATOR:         return "Network Animator";
    case JCE_COMP_FLAG_NET_RIGIDBODY:        return "Network Rigidbody";
    case JCE_COMP_FLAG_VFX_GRAPH:            return "VFX Graph";
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
    case JCE_COMP_FLAG_VFX_GRAPH:            return "comp.vfxGraph";
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
