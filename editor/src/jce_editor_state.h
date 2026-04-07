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
#define JCE_MAX_ENTITY_NAME    128
#define JCE_MAX_TAG_STRING     64
#define JCE_MAX_ENTITIES       4096
#define JCE_MAX_COMPONENTS     32

/* ── Max Children ──────────────────────────────────────────────────── */

#define JCE_MAX_CHILDREN       64

/* ── Forward-declare component type ────────────────────────────────── */

typedef struct JceComponentInfo JceComponentInfo;

/* ── Entity Data (lightweight, for editor display) ─────────────────── */

typedef struct {
    uint32_t    id;                           /* engine entity id */
    char        name[JCE_MAX_ENTITY_NAME];
    char        tag[JCE_MAX_TAG_STRING];
    JceTagColor tag_color;
    bool        enabled;
    uint32_t    parent_id;                    /* 0 = root */
    uint32_t    children[JCE_MAX_CHILDREN];   /* child entity ids */
    int         child_count;

    /* Per-entity component storage (parsed from scene JSON). */
    int         component_count;
} JceEntityInfo;

/* ── Component Data (for inspector display) ────────────────────────── */

typedef enum {
    JCE_COMP_TRANSFORM = 0,
    JCE_COMP_MESH_RENDERER,
    JCE_COMP_SPRITE_RENDERER,
    JCE_COMP_CAMERA,
    JCE_COMP_LIGHT,
    JCE_COMP_ANIMATOR,
    JCE_COMP_SKELETAL_ANIMATOR,
    JCE_COMP_RIGIDBODY,
    JCE_COMP_BOX_COLLIDER,
    JCE_COMP_SPHERE_COLLIDER,
    JCE_COMP_CHARACTER_CONTROLLER,
    JCE_COMP_AUDIO_SOURCE,
    JCE_COMP_SCRIPT,
    JCE_COMP_TYPE_COUNT,
} JceComponentType;

struct JceComponentInfo {
    JceComponentType type;
    bool             expanded;   /* fold state in inspector */
    /* Component-specific data (union for common types). */
    union {
        struct { float pos[3]; float rot[3]; float scale[3]; } transform;
        struct { char mesh_path[128]; char material_path[128]; } mesh_renderer;
        struct { float color[4]; float intensity; int type; } light;
        struct { float fov; float near_clip; float far_clip; bool ortho; } camera;
    } data;
};

/* ── Editor State API ──────────────────────────────────────────────── */

void  jce_editor_state_init(void);
void  jce_editor_state_shutdown(void);

/* Selection */
void        jce_state_select_entity(uint32_t id, bool add_to_selection);
void        jce_state_deselect_entity(uint32_t id);
void        jce_state_clear_selection(void);
bool        jce_state_is_selected(uint32_t id);
uint32_t    jce_state_get_focused(void);
const uint32_t *jce_state_get_selection(int *out_count);

/* Entity management (demo/stub data for now). */
int               jce_state_get_entity_count(void);
JceEntityInfo    *jce_state_get_entity(uint32_t id);
JceEntityInfo    *jce_state_get_entity_by_index(int index);
JceEntityInfo    *jce_state_get_root_entities(int *out_count);
uint32_t          jce_state_create_entity(const char *name, uint32_t parent_id);
void              jce_state_delete_entity(uint32_t id);
void              jce_state_rename_entity(uint32_t id, const char *name);
void              jce_state_set_entity_enabled(uint32_t id, bool enabled);
void              jce_state_set_entity_tag(uint32_t id, const char *tag);
void              jce_state_set_entity_tag_color(uint32_t id, JceTagColor color);
void              jce_state_reparent_entity(uint32_t id, uint32_t new_parent);
uint32_t          jce_state_duplicate_entity(uint32_t id);

/* Component management. */
int                  jce_state_get_components(uint32_t entity_id, JceComponentInfo *out, int max);
JceComponentInfo    *jce_state_get_entity_components(uint32_t entity_id, int *out_count);
void                 jce_state_add_component(uint32_t entity_id, JceComponentType type);
void                 jce_state_remove_component(uint32_t entity_id, JceComponentType type);
void                 jce_state_set_component(uint32_t entity_id, const JceComponentInfo *comp);
const char          *jce_component_type_name(JceComponentType type);
JceComponentType     jce_component_type_from_name(const char *name);

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
bool              jce_state_get_2d_mode(void);
void              jce_state_set_2d_mode(bool is_2d);
bool              jce_state_get_live_preview(void);
void              jce_state_set_live_preview(bool on);

/* Scene loading */
bool              jce_state_load_scene_file(const char *scene_path);
bool              jce_state_save_scene_file(const char *scene_path);
const char       *jce_state_get_current_scene_path(void);

/* Play mode */
void          jce_state_play(void);
void          jce_state_pause(void);
void          jce_state_stop(void);
JcePlayState  jce_state_get_play_state(void);

/* Undo/Redo (stub for future). */
void  jce_state_undo(void);
void  jce_state_redo(void);
bool  jce_state_can_undo(void);
bool  jce_state_can_redo(void);

/* Entity clipboard */
void     jce_state_copy_entity(uint32_t id);
uint32_t jce_state_paste_entity(uint32_t parent_id);
bool     jce_state_has_copied(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_STATE_H */
