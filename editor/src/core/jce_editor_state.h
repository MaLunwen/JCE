/*
 * jce_editor_state.h  Central editor state management.
 *
 * Ported from EditorState.java — single source of truth for selection,
 * edit/gizmo modes, play state, and entity operations.
 *
 * This is a C header with C linkage so the engine can query editor state.
 */

#ifndef JCE_EDITOR_STATE_H
#define JCE_EDITOR_STATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/scene/jce_scene.h>

/* ── Edit Mode ─────────────────────────────────────────────────────── */

typedef enum {
    JCE_EDIT_MODE_SELECT = 0,
    JCE_EDIT_MODE_MOVE,
    JCE_EDIT_MODE_ROTATE,
    JCE_EDIT_MODE_SCALE,
} JceEditMode;

/* ── Gizmo Mode ────────────────────────────────────────────────────── */

typedef enum {
    JCE_GIZMO_TRANSLATE = 0,
    JCE_GIZMO_ROTATE,
    JCE_GIZMO_SCALE,
} JceGizmoMode;

/* ── Gizmo Space ───────────────────────────────────────────────────── */

typedef enum {
    JCE_GIZMO_LOCAL = 0,
    JCE_GIZMO_WORLD,
} JceGizmoSpace;

/* ── Scene View Mode ───────────────────────────────────────────────── */

typedef enum {
    JCE_VIEW_SHADED = 0,
    JCE_VIEW_WIREFRAME,
    JCE_VIEW_TEXTURED,
    JCE_VIEW_WIREFRAME_TEXTURED,
} JceSceneViewMode;

/* ── Play State ────────────────────────────────────────────────────── */

typedef enum {
    JCE_PLAY_STOPPED = 0,
    JCE_PLAY_PLAYING,
    JCE_PLAY_PAUSED,
} JcePlayState;

/* ── Entity Tag Color (macOS Finder style) ─────────────────────────── */

typedef enum {
    JCE_TAG_NONE = 0,
    JCE_TAG_RED,
    JCE_TAG_ORANGE,
    JCE_TAG_YELLOW,
    JCE_TAG_GREEN,
    JCE_TAG_BLUE,
    JCE_TAG_PURPLE,
    JCE_TAG_GRAY,
    JCE_TAG_COLOR_COUNT,
} JceTagColor;

/* ── Max Limits ────────────────────────────────────────────────────── */

#define JCE_MAX_SELECTED       512
#define JCE_MAX_ENTITY_NAME    64
#define JCE_MAX_TAG_STRING     64
#define JCE_MAX_PREFAB_PATH    260

/* ── Max Children ──────────────────────────────────────────────────── */

#define JCE_MAX_CHILDREN       64

/* ── Editor State API ──────────────────────────────────────────────── */

void  jce_editor_state_init(void);
void  jce_editor_state_shutdown(void);

/* Selection */
void        jce_state_select_entity(uint32_t id, bool add_to_selection);
void        jce_state_deselect_entity(uint32_t id);
void        jce_state_clear_selection(void);
bool        jce_state_is_selected(uint32_t id);
uint32_t    jce_state_get_focused(void);
/* Set the focused entity *without* mutating the selection array order.
 * Caller must ensure id is currently in the selection (or 0 to clear). */
void        jce_state_set_focused(uint32_t id);
const uint32_t *jce_state_get_selection(int *out_count);

/* Entity management — ECS is the single source of truth. */
int               jce_state_get_entity_count(void);
uint32_t          jce_state_get_entity_id_by_index(int index);
bool              jce_state_entity_exists(uint32_t id);
uint32_t          jce_state_create_entity(const char *name, uint32_t parent_id);
void              jce_state_delete_entity(uint32_t id);
void              jce_state_rename_entity(uint32_t id, const char *name);
void              jce_state_set_entity_enabled(uint32_t id, bool enabled);
void              jce_state_set_entity_tag(uint32_t id, const char *tag);
void              jce_state_set_entity_tag_color(uint32_t id, JceTagColor color);
void              jce_state_reparent_entity(uint32_t id, uint32_t new_parent);
void              jce_state_reorder_sibling(uint32_t entity_id, uint32_t ref_id,
                                            bool insert_after);
uint32_t          jce_state_duplicate_entity(uint32_t id);

/* Entity property queries — read from ECS EditorMeta + scene hierarchy. */
const char       *jce_state_entity_name(uint32_t id);
bool              jce_state_entity_enabled(uint32_t id);
uint32_t          jce_state_entity_parent(uint32_t id);
int               jce_state_entity_child_count(uint32_t id);
int               jce_state_entity_children(uint32_t id, uint32_t *out, int max);
JceTagColor       jce_state_entity_tag_color(uint32_t id);
const char       *jce_state_entity_tag(uint32_t id);
bool              jce_state_entity_is_prefab(uint32_t id);
const char       *jce_state_entity_prefab_path(uint32_t id);
JceEntity         jce_state_to_ecs_entity(uint32_t id);
uint32_t          jce_state_from_ecs_entity(JceEntity e);

/* Component management (thin wrappers — uses JceComponentFlag from jce_scene.h). */
void              jce_state_add_component(uint32_t entity_id, uint32_t comp_flag);
void              jce_state_remove_component(uint32_t entity_id, uint32_t comp_flag);
const char       *jce_comp_flag_display_name(uint32_t comp_flag);

/* Root entity enumeration. */
int               jce_state_get_root_count(void);
uint32_t          jce_state_get_root_id(int index);

/* Edit mode */
void          jce_state_set_edit_mode(JceEditMode mode);
JceEditMode   jce_state_get_edit_mode(void);

/* Gizmo */
void          jce_state_set_gizmo_mode(JceGizmoMode mode);
JceGizmoMode  jce_state_get_gizmo_mode(void);
void          jce_state_set_gizmo_space(JceGizmoSpace space);
JceGizmoSpace jce_state_get_gizmo_space(void);

/* Scene view */
void              jce_state_set_view_mode(JceSceneViewMode mode);
JceSceneViewMode  jce_state_get_view_mode(void);
bool              jce_state_get_show_grid(void);
void              jce_state_set_show_grid(bool show);
bool              jce_state_get_show_physics_debug(void);
void              jce_state_set_show_physics_debug(bool show);

/* Show Flags (UE-style overlay toggles).  Bitmask, see JceShowFlag below.
 * Existing show_grid / show_physics_debug remain authoritative; the bitmask
 * is the new generic API (consumed by editor overlay code as it's wired up). */
typedef enum {
    JCE_SHOW_FLAG_GIZMOS         = 1u <<  0,
    JCE_SHOW_FLAG_LIGHT_ICONS    = 1u <<  1,
    JCE_SHOW_FLAG_CAMERA_ICONS   = 1u <<  2,
    JCE_SHOW_FLAG_COLLIDERS      = 1u <<  3,
    JCE_SHOW_FLAG_SKYBOX         = 1u <<  4,
    JCE_SHOW_FLAG_BOUNDING_BOXES = 1u <<  5,
    JCE_SHOW_FLAG_WORLD_AXIS     = 1u <<  6,
    JCE_SHOW_FLAG_STATS_OVERLAY  = 1u <<  7,
    JCE_SHOW_FLAG_NAVMESH        = 1u <<  8,
} JceShowFlag;

uint32_t          jce_state_get_show_flags(void);
void              jce_state_set_show_flags(uint32_t flags);
bool              jce_state_show_flag(JceShowFlag f);
void              jce_state_set_show_flag(JceShowFlag f, bool on);
bool              jce_state_get_2d_mode(void);
void              jce_state_set_2d_mode(bool is_2d);
bool              jce_state_get_live_preview(void);
void              jce_state_set_live_preview(bool on);

/* Scene loading */
bool              jce_state_load_scene_file(const char *scene_path);
bool              jce_state_save_scene_file(const char *scene_path);
const char       *jce_state_get_current_scene_path(void);
bool              jce_state_is_scene_modified(void);
void              jce_state_clear_scene_modified(void);

/* Play mode */
void          jce_state_play(void);
void          jce_state_pause(void);
void          jce_state_stop(void);
/* Advance the simulation by exactly one frame while paused.  No-op if not
 * currently in JCE_PLAY_PAUSED.  Useful for frame-by-frame debugging. */
void          jce_state_step(float dt);
JcePlayState  jce_state_get_play_state(void);
void          jce_state_play_mode_tick(float dt);

/* Undo/Redo history. */
void  jce_state_undo(void);
void  jce_state_redo(void);
bool  jce_state_can_undo(void);
bool  jce_state_can_redo(void);

/* Batch edit scope for grouping multi-step operations into one undo entry. */
void  jce_state_begin_batch_edit(void);
void  jce_state_end_batch_edit(void);

/* Transient edit scope: no undo snapshot, but marks scene dirty. */
void  jce_state_begin_transient_edit(void);
void  jce_state_end_transient_edit(void);

/* Explicit transaction scope for multi-step editor workflows. */
bool  jce_state_begin_transaction(const char *label);
void  jce_state_commit_transaction(void);
void  jce_state_cancel_transaction(void);
bool  jce_state_transaction_active(void);

/* Prefab lifecycle (minimum viable Phase 4 core). */
bool     jce_state_save_prefab(uint32_t entity_id, const char *prefab_path);
uint32_t jce_state_instantiate_prefab(const char *prefab_path, uint32_t parent_id);
bool     jce_state_revert_prefab(uint32_t entity_id);
bool     jce_state_is_prefab_instance(uint32_t entity_id);
const char *jce_state_get_prefab_path(uint32_t entity_id);

/* Prefab Variant (P1 #10).  Saves a snapshot annotated with `$variantOf`
 * pointing at the parent prefab.  In v0.7.12 the loader treats the
 * snapshot as authoritative (no deep merge yet) — variant inheritance
 * gives a stable on-disk format and menu entry to build on. */
bool        jce_state_save_prefab_variant(uint32_t entity_id,
                                          const char *variant_path,
                                          const char *parent_prefab_path);
const char *jce_state_get_variant_parent(uint32_t entity_id);

/* Entity clipboard */
void     jce_state_copy_entity(uint32_t id);
uint32_t jce_state_paste_entity(uint32_t parent_id);
bool     jce_state_has_copied(void);

/* Multi-entity clipboard. count<=0 clears the clipboard.  Cut mode marks
 * the entries for deletion on the next paste; callers query
 * jce_state_clipboard_is_cut() to render ghosting/labels. */
void     jce_state_copy_entities(const uint32_t *ids, int count, bool cut);
int      jce_state_clipboard_count(void);
bool     jce_state_clipboard_is_cut(void);
const uint32_t *jce_state_clipboard_source_ids(int *out_count);
/* Pastes ALL clipboard entries under parent_id (0 = root). Returns number
 * of entities pasted; out_ids (if non-NULL, capacity max_out) receives the
 * new entity IDs in clipboard order.  If the clipboard is in cut mode the
 * source entities are deleted after a successful paste and the clipboard
 * is cleared. */
int      jce_state_paste_entities(uint32_t parent_id,
                                  uint32_t *out_ids, int max_out);

/* Engine scene backing store. */
void       jce_state_set_scene(JceScene *scene);
JceScene  *jce_state_get_scene(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_STATE_H */
