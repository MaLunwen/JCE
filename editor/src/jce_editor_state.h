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

#include <jce/scene/jce_scene.h>

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
#define JCE_MAX_PREFAB_PATH    260
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
    bool        prefab_instance;
    char        prefab_path[JCE_MAX_PREFAB_PATH];

    uint64_t    ecs_entity;                   /* JceEntity in engine scene */

    /* Per-entity component storage (parsed from scene JSON). */
    int         component_count;
} JceEntityInfo;

/* ── Component Data (for inspector display) ────────────────────────── */

/* Procedural mesh shape for MeshRenderer when no mesh_path is set. */
enum {
    JCE_MESH_SHAPE_CUBE     = 0,
    JCE_MESH_SHAPE_SPHERE   = 1,
    JCE_MESH_SHAPE_PLANE    = 2,
    JCE_MESH_SHAPE_CAPSULE  = 3,
    JCE_MESH_SHAPE_CYLINDER = 4,
    JCE_MESH_SHAPE_COUNT
};

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

        struct {
            char  mesh_path[128];
            char  material_path[128];
            int   mesh_shape;         /* JCE_MESH_SHAPE_* (default 0=cube) */
            /* PBR material parameters (inline editing). */
            float base_color[4];      /* RGBA linear */
            float metallic;           /* 0..1 */
            float roughness;          /* 0..1 */
            float emissive[3];        /* RGB */
            float normal_scale;       /* default 1.0 */
            float ao_strength;        /* 0..1 */
            int   alpha_mode;         /* 0=OPAQUE, 1=MASK, 2=BLEND */
            float alpha_cutoff;       /* default 0.5 */
            bool  double_sided;
            char  albedo_tex[128];
            char  mr_tex[128];        /* metallic-roughness map */
            char  normal_tex[128];
            char  ao_tex[128];
            char  emissive_tex[128];
        } mesh_renderer;

        struct { float color[4]; float intensity; int type; } light;
        struct { float fov; float near_clip; float far_clip; bool ortho; } camera;

        struct {
            char  sprite_path[128];
            float color[4];
            bool  flip_x;
            bool  flip_y;
            int   sorting_order;
        } sprite_renderer;

        struct {
            char  clip_name[64];
            float speed;
            bool  loop;
            bool  playing;
        } animator;

        struct {
            char  skeleton_path[128];
            char  clip_names[8][64];
            int   clip_count;
            int   active_clip;
            float speed;
            bool  loop;
            bool  playing;
        } skeletal_animator;

        struct {
            float mass;
            float drag;
            float angular_drag;
            bool  use_gravity;
            bool  is_kinematic;
        } rigidbody;

        struct {
            float center[3];
            float size[3];
            bool  is_trigger;
        } box_collider;

        struct {
            float center[3];
            float radius;
            bool  is_trigger;
        } sphere_collider;

        struct {
            float height;
            float radius;
            float step_offset;
            float slope_limit;
        } character_controller;

        struct {
            char  clip_path[128];
            float volume;
            float pitch;
            float spatial_blend;
            bool  loop;
            bool  play_on_awake;
        } audio_source;

        struct {
            char  script_path[128];
        } script;
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
void              jce_state_reorder_sibling(uint32_t entity_id, uint32_t ref_id,
                                            bool insert_after);
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
bool              jce_state_is_scene_modified(void);
void              jce_state_clear_scene_modified(void);

/* Play mode */
void          jce_state_play(void);
void          jce_state_pause(void);
void          jce_state_stop(void);
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

/* Entity clipboard */
void     jce_state_copy_entity(uint32_t id);
uint32_t jce_state_paste_entity(uint32_t parent_id);
bool     jce_state_has_copied(void);

/* Engine scene backing store. */
void       jce_state_set_scene(JceScene *scene);
JceScene  *jce_state_get_scene(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_STATE_H */
