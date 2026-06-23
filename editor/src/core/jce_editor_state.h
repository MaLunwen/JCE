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

/* ── Gizmo Pivot ───────────────────────────────────────────────────── */

typedef enum {
    JCE_GIZMO_PIVOT  = 0,  /* gizmo at the active object's transform origin */
    JCE_GIZMO_CENTER,      /* gizmo at the geometric center of the selection */
} JceGizmoPivot;

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

/* Initialize editor state.  When `with_demo_scene` is true, populates the
 * default 13-entity demo scene; pass false at startup when a last scene
 * will be restored immediately, to skip building (and instantly
 * destroying) the throwaway demo — saves that work off the
 * time-to-first-frame path. */
void  jce_editor_state_init(bool with_demo_scene);
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
bool              jce_state_entity_alive(uint32_t id);          /* crash-safe liveness (list test) */
uint32_t          jce_state_find_by_name(const char *name);    /* name -> LIVE editor id, 0 if none */
void              jce_state_prune_dead(void);                   /* drop runtime-destroyed entities from the mirror list */
/* Install spawn/despawn hooks so world-streamed chunk entities are mirrored
 * into g_entity_order (Hierarchy panel + selection).  Editor preview + Play
 * call this on their streamer after creating it. */
struct JceWorldStreamer;
void              jce_state_attach_streamer_hierarchy(struct JceWorldStreamer *ws);
/* Install the HLOD far-skyline coordination: map each streaming chunk to its
 * always-resident HLOD_<gx>_<gz> proxy (baked by build/gen_hlod.py) and hide
 * the proxy while the chunk is resident / show it when it unloads.  No-op when
 * no proxies were baked.  Call after creating the streamer (preview + Play). */
void              jce_state_attach_streamer_hlod(struct JceWorldStreamer *ws);
/* Re-show all proxies and clear the map; call when the streamer is torn down. */
void              jce_state_detach_streamer_hlod(void);
uint32_t          jce_state_create_entity(const char *name, uint32_t parent_id);
void              jce_state_delete_entity(uint32_t id);
void              jce_state_rename_entity(uint32_t id, const char *name);
void              jce_state_set_entity_enabled(uint32_t id, bool enabled);
void              jce_state_set_entity_tag(uint32_t id, const char *tag);
void              jce_state_set_entity_tag_color(uint32_t id, JceTagColor color);
void              jce_state_set_entity_layer(uint32_t id, int layer);
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

/* Component management.  The dense comp_id variants (component registry,
   jce_component_registry.h) are the canonical path; the flag variants are
   thin legacy wrappers for call sites that still hold JCE_COMP_FLAG_*. */
void              jce_state_add_component_id(uint32_t entity_id, int comp_id);
void              jce_state_remove_component_id(uint32_t entity_id, int comp_id);
void              jce_state_add_component(uint32_t entity_id, uint64_t comp_flag);
void              jce_state_remove_component(uint32_t entity_id, uint64_t comp_flag);
const char       *jce_comp_flag_display_name(uint64_t comp_flag);
/* Return the i18n key (e.g. "comp.transform") for a component flag, or
   NULL when the flag has no translation key registered. The Inspector
   uses this for cross-locale search so users can find components by
   typing in any language present in the loaded locale tables. */
const char       *jce_comp_flag_i18n_key(uint64_t comp_flag);

/* Root entity enumeration. */
int               jce_state_get_root_count(void);
uint32_t          jce_state_get_root_id(int index);
/* O(n) single-pass: collect all root ids (parent==INVALID) into out (up to
 * max), in g_entity_order order.  Use this instead of the get_root_count +
 * per-index get_root_id loop, which is O(n^2) and catastrophic at full-load
 * (thousands of roots x an O(n) scan each). */
int               jce_state_get_roots(uint32_t *out, int max);

/* Edit mode */
void          jce_state_set_edit_mode(JceEditMode mode);
JceEditMode   jce_state_get_edit_mode(void);

/* Gizmo */
void          jce_state_set_gizmo_mode(JceGizmoMode mode);
JceGizmoMode  jce_state_get_gizmo_mode(void);
void          jce_state_set_gizmo_space(JceGizmoSpace space);
JceGizmoSpace jce_state_get_gizmo_space(void);
void          jce_state_set_gizmo_pivot(JceGizmoPivot pivot);
JceGizmoPivot jce_state_get_gizmo_pivot(void);
void          jce_state_set_pivot_edit_mode(bool enabled);
bool          jce_state_get_pivot_edit_mode(void);

/* Gizmo Ctrl-snap increments (translate units / rotate degrees / scale
 * ratio).  Read by the gizmo snap path; edited via the Scene View "Snap"
 * popup.  Setters persist to .jce/editor-config.json. */
float jce_state_get_gizmo_snap_translate(void);
float jce_state_get_gizmo_snap_rotate(void);
float jce_state_get_gizmo_snap_scale(void);
void  jce_state_set_gizmo_snap_translate(float v);
void  jce_state_set_gizmo_snap_rotate(float v);
void  jce_state_set_gizmo_snap_scale(float v);

/* Scene view */
void              jce_state_set_view_mode(JceSceneViewMode mode);
JceSceneViewMode  jce_state_get_view_mode(void);
bool              jce_state_get_show_grid(void);
void              jce_state_set_show_grid(bool show);
bool              jce_state_get_show_physics_debug(void);
void              jce_state_set_show_physics_debug(bool show);
bool              jce_state_get_show_joint_gizmos(void);
void              jce_state_set_show_joint_gizmos(bool show);
bool              jce_state_get_show_cloth_gizmos(void);
void              jce_state_set_show_cloth_gizmos(bool show);
/* World-streaming preview (session-local, default off): when on AND the
 * scene's streaming settings are enabled, the editor runs a live preview
 * streamer that spawns/destroys chunk entities in the hierarchy as the
 * editor camera moves.  See jce_editor_scene_render_streaming_rebuild(). */
bool              jce_state_get_streaming_preview(void);
void              jce_state_set_streaming_preview(bool on);

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
    JCE_SHOW_FLAG_STREAMING      = 1u <<  9,
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
bool              jce_state_new_default_scene(void);
bool              jce_state_load_scene_file(const char *scene_path);

/* Frame-sliced (non-blocking) scene-file open.  For large scenes (entity
 * count over an internal threshold) this parses the file, then creates the
 * entities in chunks across subsequent jce_state_scene_load_poll() calls so
 * the editor's frame loop stays alive and a "Loading…" overlay can show
 * progress.  Small scenes load synchronously (identical to
 * jce_state_load_scene_file) and report not-loading immediately.
 * Returns true if the open started (or completed synchronously), false on a
 * read/parse failure (in which case the previous scene is left intact). */
bool              jce_state_load_scene_file_async(const char *scene_path);
/* Drive a pending async scene load — call once per editor frame.  No-op when
 * nothing is loading.  Runs the post-load finalize (hierarchy mirror, camera,
 * recents, streaming autostart) only when the create pass is fully done. */
void              jce_state_scene_load_poll(void);
/* True while an async scene load is mid-flight (entities still being created).
 * Editor interaction should be gated (via a modal overlay) while true so
 * nothing operates on a half-built scene. */
bool              jce_state_is_scene_loading(void);
/* 0..1 progress of the in-flight async load (0 when none). */
float             jce_state_scene_load_progress(void);
/* Load a scene from a standalone .jbundle (single-file mode product).
 * Mounts the bundle internally, reads the scene JSON through the VFS,
 * and applies it as the current scene.  The bundle stays mounted (so
 * referenced assets can resolve) until the next bundle/scene load.
 * The current_scene_path is set to "bundle://<jbundle>" — saving is
 * disabled in this mode (the bundle is read-only). */
bool              jce_state_load_scene_from_jbundle(const char *jbundle_path);
/* Load a scene from a bundle_catalog.json + bundle id (or scene vpath).
 * If bundle_id_or_scene is NULL, the first bundle in the catalog is
 * picked.  Behaves like jce_state_load_scene_from_jbundle wrt mounting
 * and read-only semantics. */
bool              jce_state_load_scene_from_catalog(const char *catalog_path,
                                                    const char *bundle_id_or_scene);
bool              jce_state_save_scene_file(const char *scene_path);
/* Per-frame pump for background scene-serial jobs (async post-save mesh
 * validation).  Call once per editor frame from the main loop. */
void              jce_state_scene_serial_poll(void);
const char       *jce_state_get_current_scene_path(void);
bool              jce_state_is_scene_modified(void);
void              jce_state_mark_scene_modified(void);
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

/* Player-character input bridge (Game View panel → Play tick).
 * Push the desired walk DIRECTION (world-space horizontal, unit length)
 * and button state each frame the panel is captured + Play is active —
 * speeds come from the authored CharacterController component.  Ignored
 * if no scene entity has a CharacterController. */
void jce_editor_play_set_player_input(float walk_x, float walk_z,
                                       bool jump_pressed, bool jump_held,
                                       bool sprint, bool attack);

/* Returns true and writes the player character's world position if a
 * character is alive; false otherwise. */
bool jce_editor_play_get_player_position(float *out_x, float *out_y, float *out_z);
bool jce_editor_play_get_player_forward(float *out_x, float *out_y, float *out_z);

/* Returns the live physics world during Play (or NULL if not running).
 * Editor-side debug-draw / contact-listener wiring uses this. */
struct JcePhysicsWorld;
struct JcePhysicsWorld *jce_editor_play_get_physics_world(void);

/* Returns the live play-session runtime during Play (or NULL when not
 * running).  Read-only panel wiring (e.g. the BT Visualizer polling
 * jce_runtime_bt_* accessors) uses this. */
struct JceRuntime;
struct JceRuntime *jce_editor_play_get_runtime(void);

/* Top 6: bind the live editor action map into the Play runtime each frame so
 * gameplay scripts can read authored actions by name (jce.is_action_down /
 * get_axis) in editor Play, exactly like a shipped game.  No-op when not in
 * Play.  Pass the result of jce_editor_input_actions_live(). */
struct JceInputActions;
void jce_editor_play_set_actions(const struct JceInputActions *actions);

/* Live count of active contact pairs during Play (BEGIN++/END--).  0 when
 * not running.  Surfaced by the Physics Debugger. */
int jce_editor_play_get_active_contacts(void);

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
/* Push this instance's edits back to its source .prefab.json (Phase 0.3). */
bool     jce_state_apply_prefab(uint32_t entity_id);
bool     jce_state_is_prefab_instance(uint32_t entity_id);
const char *jce_state_get_prefab_path(uint32_t entity_id);

/* Per-COMPONENT prefab override ops (Unity/Godot-style).  Apply pushes one
 * component (canonical engine name, e.g. "Transform") from the instance
 * back to its source .prefab.json and updates every sibling instance that
 * was not overriding that component; Revert copies the component from the
 * source back onto the instance (dropping the override).  Both are atomic
 * (one undo entry).  is_*_overridden answers the inspector indicator. */
bool     jce_state_apply_prefab_component(uint32_t entity_id,
                                          const char *comp_name);
bool     jce_state_revert_prefab_component(uint32_t entity_id,
                                           const char *comp_name);
bool     jce_state_is_prefab_component_overridden(uint32_t entity_id,
                                                  const char *comp_name);
/* Batch: fill names[][64] with all overridden component names; returns the
 * count or -1 when not an instance / source unavailable.  Cheaper than the
 * per-component query for the inspector's per-frame indicator set. */
int      jce_state_get_prefab_overrides(uint32_t entity_id,
                                        char names[][64], int max_names);

/* Per-FIELD detail for the inspector override badge tooltip: fill
 * fields[][64] with the serialized field KEYS of `comp_name` that diverge
 * from the source prefab (e.g. "posX","metallic").  Returns the count, 0
 * when the component is overridden as a WHOLE (no per-field localisation,
 * e.g. the unified "Light"), or -1 when not an instance / source missing.
 * Lighter than a full save — used only for the hovered component header. */
int      jce_state_get_prefab_field_overrides(uint32_t entity_id,
                                              const char *comp_name,
                                              char fields[][64], int max_fields);

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
