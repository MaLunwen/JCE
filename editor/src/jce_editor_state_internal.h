/*
 * jce_editor_state_internal.h  Shared internal declarations for editor state.
 *
 * Included by all jce_editor_state_*.cpp files.  Not part of the public API.
 * Contains the editor internal state struct, history types, and forward
 * declarations for cross-file functions.
 */

#ifndef JCE_EDITOR_STATE_INTERNAL_H
#define JCE_EDITOR_STATE_INTERNAL_H

#include "jce_editor_state.h"
#include "jce_editor_defaults.h"
#include "jce_editor_alloc.h"

#include <cjson/cJSON.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <vector>
#include <string>
#include <utility>

extern "C" {
#include <jce/core/jce_log.h>
#include <jce/core/jce_math.h>
#include <jce/resource/jce_scene_contract.h>
#include <jce/scene/jce_scene.h>
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
	JceSceneViewMode view_mode;
	JcePlayState     play_state;
	bool             show_grid;
	bool             is_2d_mode;
	bool             live_preview;

	/* Entity storage (demo data, replaced by ECS later). */
	JceEntityInfo    entities[JCE_MAX_ENTITIES];
	JceComponentInfo components[JCE_MAX_ENTITIES][JCE_MAX_COMPONENTS];
	int              entity_count;
	uint32_t         next_id;
	JceScene        *scene;                    /* engine ECS backing store */
	char             current_scene_path[512];

	bool initialized;
};

extern EditorInternalState s;

/* ── History Snapshot ──────────────────────────────────────────────── */

struct EditorHistorySnapshot {
	std::string scene_json;
	std::string scene_path;
};

extern std::vector<EditorHistorySnapshot> s_undo_history;
extern std::vector<EditorHistorySnapshot> s_redo_history;
extern int  s_history_suspend_depth;
extern int  s_history_edit_nesting;
extern bool s_history_outer_edit_pushed_snapshot;
extern int  s_history_manual_batch_depth;

/* ── Transaction ──────────────────────────────────────────────────── */

struct EditorTransaction {
	bool active;
	char label[64];
	EditorHistorySnapshot before;
};

extern EditorTransaction s_transaction;

/* ── Core helpers (defined in jce_editor_state.cpp) ───────────────── */

int  find_entity(uint32_t id);
void set_current_scene_path_internal(const char *scene_path);
void update_scene_dir_from_path(const char *scene_path);
void clear_scene_entities(void);

/* ── History functions (defined in jce_editor_history.cpp) ─────────── */

bool history_begin_edit(void);
void history_end_edit(bool active);
bool history_capture_snapshot(EditorHistorySnapshot *out);
bool history_push_undo_snapshot(void);
bool history_restore_snapshot(const EditorHistorySnapshot &snapshot,
                              const char *reason);

/* ── Scene parse functions (defined in jce_editor_scene_parse.cpp) ── */

JceComponentType component_type_from_name(const char *name);
uint32_t load_entity_tree_node(const cJSON *node, uint32_t parent_id);
bool     looks_like_entity_object(const cJSON *obj);
bool     parse_scene_contract_version(const cJSON *root,
                                      int *out_major, int *out_minor);
bool     load_scene_from_parsed_root(const cJSON *root,
                                     const char *scene_label,
                                     const char *scene_path);

/* ── Scene serial functions (defined in jce_editor_scene_serial.cpp) ─ */

cJSON       *serialize_component_json(const JceComponentInfo *comp);
cJSON       *serialize_entity_tree_json(uint32_t entity_id);
cJSON       *build_scene_json_root(void);
cJSON       *build_prefab_json_root(uint32_t entity_id);
const cJSON *find_prefab_root_node(const cJSON *root);
void         mark_prefab_instance_recursive(uint32_t entity_id,
                                            const char *prefab_path);

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
