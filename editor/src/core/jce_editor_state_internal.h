/*
 * jce_editor_state_internal.h  Shared internal declarations for editor state.
 *
 * Included by all jce_editor_state_*.cpp files.  Not part of the public API.
 * Contains the editor internal state struct, history types, and forward
 * declarations for cross-file functions.
 *
 * After the ECS-direct refactor, the engine's JceScene (flecs) is the single
 * source of truth for all entity / component data.  The editor keeps only
 * UI-specific state (selection, modes, undo history, sidecar fold flags).
 */

#ifndef JCE_EDITOR_STATE_INTERNAL_H
#define JCE_EDITOR_STATE_INTERNAL_H

#include "jce_editor_alloc.h"
#include "jce_editor_defaults.h"
#include "jce_editor_state.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/resource/jce_scene_contract.h>
#include <jce/resource/jce_scene_serial.h>
}

#define LOG_TAG "editor_state"

/* ── Internal State ────────────────────────────────────────────────── */

struct EditorInternalState {
    /* Selection. */
    uint32_t    selected[JCE_MAX_SELECTED];
    int         selected_count;
    uint32_t    focused;       /* primary selection */

    /* Modes. */
    JceEditMode      edit_mode;
    JceGizmoMode     gizmo_mode;
    JceGizmoSpace    gizmo_space;
    JceGizmoPivot    gizmo_pivot;
    JceSceneViewMode view_mode;
    JcePlayState     play_state;
    bool             show_grid;
    bool             is_2d_mode;
    bool             pivot_edit_mode;
    bool             live_preview;
    bool             scene_modified;

    /* ECS backing store – the single source of truth. */
    JceScene        *scene;
    char             current_scene_path[512];

    /* Scene-file watch (core/jce_editor_scene_file_watch.cpp).  HERE rather
     * than as a file-scope object in that translation unit, for the reason
     * this folder's charter gives: all editor state goes through this struct.
     * The dedup audit agrees and counts the difference -- a static there was
     * global-state 859 -> 860, and that baseline is not for raising. */
    char             scene_watch_path[512];
    int64_t          scene_watch_mtime;
    int              scene_watch_countdown;
    bool             scene_watch_armed;
    bool             scene_watch_stale;

    bool initialized;
};

extern EditorInternalState s;

/* ── Entity order list (kept in sync with ECS) ────────────────────── */
/* Maintains creation / load order for index-based iteration.
   Rebuilt from ECS on scene load / undo; updated on create/delete. */
extern std::vector<uint32_t> g_entity_order;
/* Bumped on EVERY g_entity_order mutation (spawn/despawn/prune/clear/
 * streamed add-remove).  Consumers (hierarchy rebuild gate) fold it into
 * their change keys; reparent/rename do NOT touch the order and are
 * covered by the scene structural_epoch / their own signals instead. */
extern uint64_t g_entity_order_gen;

/* ── Editor per-entity sidecar (UI-only state) ────────────────────── */
struct EditorEntitySidecar {
    /* Inspector fold state — one bit per dense engine comp_id
       (jce_component_registry.h), default all-expanded.  4×64 bits covers
       JCE_COMP_MAX (256) registered component types, so EVERY component
       (including the post-64 rows that had no flag bit) persists its
       collapsed state.  Runtime-only: rebuilt per scene session, never
       serialized (matches the old single-word mask). */
    uint64_t expanded[4] = { ~0ull, ~0ull, ~0ull, ~0ull };
    /* Inspector display order of components (one dense engine comp_id per
       visible component; the light group uses the unified "Light" row id).
       Empty until first inspector pass.  Reordered via popup Move Up/Down
       or drag-and-drop on header.  Missing ids fall back to default order.
       Runtime-only: captured into undo snapshots, never written to disk. */
    std::vector<int> component_order;
};
extern std::unordered_map<uint32_t, EditorEntitySidecar> g_entity_sidecar;

/* ── History Snapshot ──────────────────────────────────────────────── */

struct EditorHistorySnapshot {
    uint64_t sequence = 0;

    /* ENTITY-SCOPED RECORD when entity_id != 0.
     *
     * A full record serialises the whole scene -- 26 MB and 715 ms at 50k
     * entities, twice per edit (once to push, once to compare) -- and restores
     * by clearing the scene, which also drops the selection and every entity
     * handle.  An inspector edit changes one entity, so it records one:
     *   entity_json      the entity's "components" array at capture time
     *   entity_comp_ids  the component SET, so a restore can REMOVE a
     *                    component the edit added (parse alone only adds)
     *   entity_order     that entity's inspector display order
     * scene_json is empty in this case and scene_path still carries the
     * scene identity, so the two kinds share one stack and one ordering. */
    uint32_t entity_id = 0;
    std::string entity_json;
    std::vector<int> entity_comp_ids;
    std::vector<int> entity_order;

    std::string scene_json;
    std::string scene_path;
    std::vector<uint32_t> selection;
    uint32_t focused = 0;
    /* Per-entity Inspector component display order (dense comp_ids).
       Captured alongside the scene so undo/redo of Move Up/Move Down/
       drag-reorder restores the prior layout. Sidecar fold-state is
       intentionally NOT undoable (matches Unity behaviour). */
    std::unordered_map<uint32_t, std::vector<int>> component_orders;
};

extern std::vector<EditorHistorySnapshot> s_undo_history;
extern std::vector<EditorHistorySnapshot> s_redo_history;
extern int  s_history_suspend_depth;
extern int  s_history_edit_nesting;
extern bool s_history_outer_edit_pushed_snapshot;
extern int  s_history_manual_batch_depth;
extern int  s_history_transient_batch_depth;

/* ── Transaction ──────────────────────────────────────────────────── */

struct EditorTransaction {
    bool active;
    char label[64];
    EditorHistorySnapshot before;
};

extern EditorTransaction s_transaction;

/* ── Core helpers (defined in jce_editor_state.cpp) ───────────────── */

void set_current_scene_path_internal(const char *scene_path);
void update_scene_dir_from_path(const char *scene_path);
void jce_roots_invalidate(void);
void clear_scene_entities(void);
void rebuild_entity_order_from_ecs(void);

/* Auto-stop play mode before a scene swap (new / open / load).
 *
 * Creating a new scene or opening/loading a scene destroys and recreates
 * the JceScene that a running runtime still references, which is a
 * use-after-free during Play.  Call this at the START of every
 * user-facing scene-swap entry point so the runtime is torn down first.
 * Safe (no-op) when already stopped.  Defined in jce_editor_play.cpp.
 *
 * NOTE: do NOT call this from clear_scene_entities() itself or from the
 * history/undo path — clear_scene_entities() is legitimately invoked
 * during jce_state_stop()'s snapshot restore, when the runtime is gone. */
void stop_play_before_scene_swap(void);

/* ── History functions (defined in jce_editor_history.cpp) ─────────── */

bool history_begin_edit(void);
void history_end_edit(bool active);
bool history_capture_snapshot(EditorHistorySnapshot *out);
bool history_push_undo_snapshot(void);
bool history_push_undo_snapshot_scoped(uint32_t entity_id);
bool history_capture_entity_snapshot(uint32_t entity_id,
                                     EditorHistorySnapshot *out);
bool history_begin_edit_scoped(uint32_t entity_id);
bool history_restore_snapshot(const EditorHistorySnapshot &snapshot,
                              const char *reason);

/* ── Scene parse functions (defined in jce_editor_scene_parse.cpp) ── */

uint32_t load_entity_tree_node(const JceJson *node, uint32_t parent_id);
bool     looks_like_entity_object(const JceJson *obj);
bool     parse_scene_contract_version(const JceJson *root,
                                       int *out_major, int *out_minor);
bool     load_scene_from_parsed_root(const JceJson *root,
                                     const char *scene_label,
                                     const char *scene_path);

/* ── Scene serial functions (defined in jce_editor_scene_serial.cpp) ─ */

JceJson       *serialize_entity_tree_json(uint32_t entity_id);
/* Override-aware variant: prefab instances are embedded as additive
 * "overrides" diffs vs their source .prefab.json (per-component, byte
 * diff).  serialize_entity_tree_json keeps the legacy full snapshot. */
JceJson       *serialize_entity_tree_json_overrides(uint32_t entity_id);
JceJson       *build_scene_json_root(void);
JceJson       *build_prefab_json_root(uint32_t entity_id);
const JceJson *find_prefab_root_node(const JceJson *root);
void         mark_prefab_instance_recursive(uint32_t entity_id,
                                            const char *prefab_path);

/* ── Euler ↔ Quaternion helpers (degrees) ─────────────────────────── */

#include "core/jce_editor_quat.h"

/* ── RAII Scopes ──────────────────────────────────────────────────── */

struct HistorySuspendScope {
    HistorySuspendScope() { ++s_history_suspend_depth; }
    ~HistorySuspendScope() {
        if (s_history_suspend_depth > 0)
            --s_history_suspend_depth;
    }
};

struct HistoryEditScope {
    bool active;
    HistoryEditScope() : active(history_begin_edit()) {}
    ~HistoryEditScope() { history_end_edit(active); }
};

#endif /* JCE_EDITOR_STATE_INTERNAL_H */
