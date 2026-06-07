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
    bool             live_preview;
    bool             scene_modified;

    /* ECS backing store – the single source of truth. */
    JceScene        *scene;
    char             current_scene_path[512];

    bool initialized;
};

extern EditorInternalState s;

/* ── Entity order list (kept in sync with ECS) ────────────────────── */
/* Maintains creation / load order for index-based iteration.
   Rebuilt from ECS on scene load / undo; updated on create/delete. */
extern std::vector<uint32_t> g_entity_order;

/* ── Editor per-entity sidecar (UI-only state) ────────────────────── */
struct EditorEntitySidecar {
    uint64_t expanded_flags = 0xFFFFFFFFFFFFFFFFull; /* Inspector fold state bitmask */
    /* Non-flag editor component slots cannot live in expanded_flags because
       they are not single-bit masks. Empty means every synthetic slot is open. */
    std::vector<uint64_t> collapsed_component_slots;
    /* Inspector display order of components (one slot per visible component).
       Empty until first inspector pass; entries are component slots, which
       are usually JCE_COMP_FLAG_* values but may be editor-only synthetic
       slots. Reordered via popup Move Up/Down or drag-and-drop on header.
       Missing slots fall back to default order. */
    std::vector<uint64_t> component_order;
};
extern std::unordered_map<uint32_t, EditorEntitySidecar> g_entity_sidecar;

/* ── History Snapshot ──────────────────────────────────────────────── */

struct EditorHistorySnapshot {
    std::string scene_json;
    std::string scene_path;
    /* Per-entity Inspector component display order. Captured alongside
       the scene so undo/redo of Move Up/Move Down/drag-reorder restores
       the prior layout. Sidecar fold-state is intentionally NOT undoable
       (matches Unity behaviour). */
    std::unordered_map<uint32_t, std::vector<uint64_t>> component_orders;
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
