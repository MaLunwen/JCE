/*
 * jce_editor_state.cpp  Central editor state implementation.
 *
 * Manages entity selection, edit modes, play state, and demo scene data.
 * When ECS integration is added (Phase 3), the entity storage will be
 * replaced by queries into the actual ECS world.
 */

#include "jce_editor_state.h"

#include <string.h>
#include <stdio.h>

extern "C" {
#include <jce/core/jce_log.h>
}

#define LOG_TAG "editor_state"

/* ── Internal State ────────────────────────────────────────────────── */

static struct {
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

    /* Entity storage (demo data, replaced by ECS later). */
    JceEntityInfo entities[JCE_MAX_ENTITIES];
    int           entity_count;
    uint32_t      next_id;

    bool initialized;
} s;

/* ── Helper: find entity index by id ───────────────────────────────── */

static int find_entity(uint32_t id)
{
    for (int i = 0; i < s.entity_count; i++)
        if (s.entities[i].id == id) return i;
    return -1;
}

/* ── Init / Shutdown ───────────────────────────────────────────────── */

void jce_editor_state_init(void)
{
    memset(&s, 0, sizeof(s));
    s.edit_mode   = JCE_EDIT_MODE_SELECT;
    s.gizmo_mode  = JCE_GIZMO_TRANSLATE;
    s.gizmo_space = JCE_GIZMO_LOCAL;
    s.view_mode   = JCE_VIEW_SHADED;
    s.play_state  = JCE_PLAY_STOPPED;
    s.show_grid   = true;
    s.next_id     = 1;

    /* Create demo scene hierarchy (matches reference editor default). */
    uint32_t root   = jce_state_create_entity("Scene Root", 0);

    uint32_t cam    = jce_state_create_entity("Main Camera", root);
    (void)cam;

    uint32_t lights = jce_state_create_entity("Lights", root);
    jce_state_create_entity("Directional Light", lights);
    jce_state_create_entity("Point Light", lights);

    uint32_t objs   = jce_state_create_entity("Objects", root);
    jce_state_create_entity("Cube", objs);
    jce_state_create_entity("Sphere", objs);
    jce_state_create_entity("Plane", objs);

    uint32_t ui     = jce_state_create_entity("UI", root);
    jce_state_create_entity("Canvas", ui);

    s.initialized = true;
    LOG_INFO(LOG_TAG, "editor state initialized (%d demo entities)", s.entity_count);
}

void jce_editor_state_shutdown(void)
{
    memset(&s, 0, sizeof(s));
    LOG_INFO(LOG_TAG, "editor state shutdown");
}

/* ── Selection ─────────────────────────────────────────────────────── */

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
            s.selected[i] = s.selected[--s.selected_count];
            if (s.focused == id)
                s.focused = s.selected_count > 0 ? s.selected[0] : 0;
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

const uint32_t *jce_state_get_selection(int *out_count)
{
    if (out_count) *out_count = s.selected_count;
    return s.selected;
}

/* ── Entity Management ─────────────────────────────────────────────── */

int jce_state_get_entity_count(void) { return s.entity_count; }

JceEntityInfo *jce_state_get_entity(uint32_t id)
{
    int idx = find_entity(id);
    return idx >= 0 ? &s.entities[idx] : NULL;
}

JceEntityInfo *jce_state_get_root_entities(int *out_count)
{
    /* Build flat list of root entities (parent_id == 0).
     * For simplicity, return pointer into entities array;
     * caller should iterate with jce_state_get_entity_count(). */
    static uint32_t roots[JCE_MAX_ENTITIES];
    static JceEntityInfo *root_ptrs[JCE_MAX_ENTITIES];
    int n = 0;
    for (int i = 0; i < s.entity_count; i++) {
        if (s.entities[i].parent_id == 0) {
            root_ptrs[n] = &s.entities[i];
            roots[n++] = s.entities[i].id;
        }
    }
    if (out_count) *out_count = n;
    /* Return first root — caller iterates via get_entity on children. */
    return n > 0 ? &s.entities[find_entity(roots[0])] : NULL;
}

uint32_t jce_state_create_entity(const char *name, uint32_t parent_id)
{
    if (s.entity_count >= JCE_MAX_ENTITIES) return 0;

    JceEntityInfo *e = &s.entities[s.entity_count++];
    memset(e, 0, sizeof(*e));
    e->id = s.next_id++;
    snprintf(e->name, sizeof(e->name), "%s", name ? name : "Entity");
    e->enabled   = true;
    e->parent_id = parent_id;
    e->tag_color = JCE_TAG_NONE;

    /* Add to parent's children list. */
    if (parent_id != 0) {
        JceEntityInfo *parent = jce_state_get_entity(parent_id);
        if (parent && parent->child_count < JCE_MAX_COMPONENTS)
            parent->children[parent->child_count++] = e->id;
    }

    return e->id;
}

void jce_state_delete_entity(uint32_t id)
{
    int idx = find_entity(id);
    if (idx < 0) return;

    /* Remove from parent's children list. */
    JceEntityInfo *e = &s.entities[idx];
    if (e->parent_id != 0) {
        JceEntityInfo *parent = jce_state_get_entity(e->parent_id);
        if (parent) {
            for (int i = 0; i < parent->child_count; i++) {
                if (parent->children[i] == id) {
                    parent->children[i] = parent->children[--parent->child_count];
                    break;
                }
            }
        }
    }

    /* Recursively delete children. */
    for (int i = e->child_count - 1; i >= 0; i--)
        jce_state_delete_entity(e->children[i]);

    /* Remove from selection. */
    jce_state_deselect_entity(id);

    /* Swap-remove from array. */
    idx = find_entity(id);  /* re-find after recursive deletes */
    if (idx >= 0) {
        s.entities[idx] = s.entities[--s.entity_count];
    }
}

void jce_state_rename_entity(uint32_t id, const char *name)
{
    JceEntityInfo *e = jce_state_get_entity(id);
    if (e) snprintf(e->name, sizeof(e->name), "%s", name);
}

void jce_state_set_entity_enabled(uint32_t id, bool enabled)
{
    JceEntityInfo *e = jce_state_get_entity(id);
    if (e) e->enabled = enabled;
}

void jce_state_set_entity_tag(uint32_t id, const char *tag)
{
    JceEntityInfo *e = jce_state_get_entity(id);
    if (e) snprintf(e->tag, sizeof(e->tag), "%s", tag ? tag : "");
}

void jce_state_set_entity_tag_color(uint32_t id, JceTagColor color)
{
    JceEntityInfo *e = jce_state_get_entity(id);
    if (e) e->tag_color = color;
}

void jce_state_reparent_entity(uint32_t id, uint32_t new_parent)
{
    JceEntityInfo *e = jce_state_get_entity(id);
    if (!e || e->id == new_parent) return;

    /* Cycle detection: ensure new_parent is not a descendant of id. */
    uint32_t check = new_parent;
    while (check != 0) {
        if (check == id) return;  /* would create cycle */
        JceEntityInfo *p = jce_state_get_entity(check);
        check = p ? p->parent_id : 0;
    }

    /* Remove from old parent. */
    if (e->parent_id != 0) {
        JceEntityInfo *old_p = jce_state_get_entity(e->parent_id);
        if (old_p) {
            for (int i = 0; i < old_p->child_count; i++) {
                if (old_p->children[i] == id) {
                    old_p->children[i] = old_p->children[--old_p->child_count];
                    break;
                }
            }
        }
    }

    /* Add to new parent. */
    e->parent_id = new_parent;
    if (new_parent != 0) {
        JceEntityInfo *new_p = jce_state_get_entity(new_parent);
        if (new_p && new_p->child_count < JCE_MAX_COMPONENTS)
            new_p->children[new_p->child_count++] = id;
    }
}

uint32_t jce_state_duplicate_entity(uint32_t id)
{
    JceEntityInfo *src = jce_state_get_entity(id);
    if (!src) return 0;

    char dup_name[JCE_MAX_ENTITY_NAME];
    snprintf(dup_name, sizeof(dup_name), "%s (Copy)", src->name);
    uint32_t dup = jce_state_create_entity(dup_name, src->parent_id);

    JceEntityInfo *d = jce_state_get_entity(dup);
    if (d) {
        d->tag_color = src->tag_color;
        d->enabled   = src->enabled;
        snprintf(d->tag, sizeof(d->tag), "%s", src->tag);
    }

    return dup;
}

/* ── Components (stub for demo) ────────────────────────────────────── */

static const char *s_comp_names[] = {
    "Transform",
    "Mesh Renderer",
    "Sprite Renderer",
    "Camera",
    "Light",
    "Animator",
    "Skeletal Animator",
    "Rigidbody",
    "Box Collider",
    "Sphere Collider",
    "Character Controller",
    "Audio Source",
    "Script",
};

const char *jce_component_type_name(JceComponentType type)
{
    if (type >= 0 && type < JCE_COMP_TYPE_COUNT)
        return s_comp_names[type];
    return "Unknown";
}

int jce_state_get_components(uint32_t entity_id, JceComponentInfo *out, int max)
{
    /* Demo: every entity gets a Transform component. */
    if (max < 1 || !out) return 0;

    memset(&out[0], 0, sizeof(out[0]));
    out[0].type = JCE_COMP_TRANSFORM;
    out[0].expanded = true;
    out[0].data.transform.scale[0] = 1.0f;
    out[0].data.transform.scale[1] = 1.0f;
    out[0].data.transform.scale[2] = 1.0f;

    int n = 1;

    /* Add a Light component to entities named "Light". */
    JceEntityInfo *e = jce_state_get_entity(entity_id);
    if (e && strstr(e->name, "Light") && n < max) {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].type = JCE_COMP_LIGHT;
        out[n].expanded = true;
        out[n].data.light.color[0] = 1.0f;
        out[n].data.light.color[1] = 1.0f;
        out[n].data.light.color[2] = 1.0f;
        out[n].data.light.color[3] = 1.0f;
        out[n].data.light.intensity = 1.0f;
        n++;
    }

    /* Add Camera to "Main Camera". */
    if (e && strstr(e->name, "Camera") && n < max) {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].type = JCE_COMP_CAMERA;
        out[n].expanded = true;
        out[n].data.camera.fov = 60.0f;
        out[n].data.camera.near_clip = 0.1f;
        out[n].data.camera.far_clip = 1000.0f;
        n++;
    }

    /* Add Mesh Renderer to geometric objects. */
    if (e && (strstr(e->name, "Cube") || strstr(e->name, "Sphere") ||
              strstr(e->name, "Plane")) && n < max) {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].type = JCE_COMP_MESH_RENDERER;
        out[n].expanded = true;
        snprintf(out[n].data.mesh_renderer.mesh_path,
                 sizeof(out[n].data.mesh_renderer.mesh_path),
                 "meshes/%s.mesh", e->name);
        snprintf(out[n].data.mesh_renderer.material_path,
                 sizeof(out[n].data.mesh_renderer.material_path),
                 "materials/default.mat");
        n++;
    }

    return n;
}

void jce_state_add_component(uint32_t entity_id, JceComponentType type)
{
    LOG_INFO(LOG_TAG, "add component %s to entity %u",
             jce_component_type_name(type), entity_id);
    /* Stub: real implementation with ECS in Phase 3. */
}

void jce_state_remove_component(uint32_t entity_id, JceComponentType type)
{
    LOG_INFO(LOG_TAG, "remove component %s from entity %u",
             jce_component_type_name(type), entity_id);
}

/* ── Mode Accessors ────────────────────────────────────────────────── */

void          jce_state_set_edit_mode(JceEditMode m)       { s.edit_mode = m; }
JceEditMode   jce_state_get_edit_mode(void)                { return s.edit_mode; }

void          jce_state_set_gizmo_mode(JceGizmoMode m)     { s.gizmo_mode = m; }
JceGizmoMode  jce_state_get_gizmo_mode(void)               { return s.gizmo_mode; }

void          jce_state_set_gizmo_space(JceGizmoSpace sp)  { s.gizmo_space = sp; }
JceGizmoSpace jce_state_get_gizmo_space(void)              { return s.gizmo_space; }

void              jce_state_set_view_mode(JceSceneViewMode m)  { s.view_mode = m; }
JceSceneViewMode  jce_state_get_view_mode(void)                { return s.view_mode; }

bool  jce_state_get_show_grid(void)          { return s.show_grid; }
void  jce_state_set_show_grid(bool show)     { s.show_grid = show; }

/* ── Play Mode ─────────────────────────────────────────────────────── */

void jce_state_play(void)
{
    if (s.play_state == JCE_PLAY_STOPPED) {
        s.play_state = JCE_PLAY_PLAYING;
        LOG_INFO(LOG_TAG, "play mode started");
    }
}

void jce_state_pause(void)
{
    if (s.play_state == JCE_PLAY_PLAYING) {
        s.play_state = JCE_PLAY_PAUSED;
        LOG_INFO(LOG_TAG, "play mode paused");
    } else if (s.play_state == JCE_PLAY_PAUSED) {
        s.play_state = JCE_PLAY_PLAYING;
        LOG_INFO(LOG_TAG, "play mode resumed");
    }
}

void jce_state_stop(void)
{
    if (s.play_state != JCE_PLAY_STOPPED) {
        s.play_state = JCE_PLAY_STOPPED;
        LOG_INFO(LOG_TAG, "play mode stopped");
    }
}

JcePlayState jce_state_get_play_state(void) { return s.play_state; }

/* ── Entity Clipboard ──────────────────────────────────────────────── */

static struct {
    uint32_t    id;
    char        name[JCE_MAX_ENTITY_NAME];
    JceTagColor tag_color;
    char        tag[JCE_MAX_TAG_STRING];
} s_clipboard;

void jce_state_copy_entity(uint32_t id)
{
    JceEntityInfo *e = jce_state_get_entity(id);
    if (!e) return;
    s_clipboard.id = id;
    snprintf(s_clipboard.name, sizeof(s_clipboard.name), "%s", e->name);
    s_clipboard.tag_color = e->tag_color;
    snprintf(s_clipboard.tag, sizeof(s_clipboard.tag), "%s", e->tag);
    LOG_INFO(LOG_TAG, "copied entity %u (%s)", id, e->name);
}

uint32_t jce_state_paste_entity(uint32_t parent_id)
{
    if (s_clipboard.id == 0) return 0;

    char paste_name[JCE_MAX_ENTITY_NAME];
    snprintf(paste_name, sizeof(paste_name), "%s (Paste)", s_clipboard.name);
    uint32_t new_id = jce_state_create_entity(paste_name, parent_id);

    JceEntityInfo *e = jce_state_get_entity(new_id);
    if (e) {
        e->tag_color = s_clipboard.tag_color;
        snprintf(e->tag, sizeof(e->tag), "%s", s_clipboard.tag);
    }

    LOG_INFO(LOG_TAG, "pasted entity as %u (%s)", new_id, paste_name);
    return new_id;
}

bool jce_state_has_copied(void)
{
    return s_clipboard.id != 0;
}

/* ── Undo / Redo (stubs) ──────────────────────────────────────────── */

void  jce_state_undo(void)      { LOG_INFO(LOG_TAG, "undo (stub)"); }
void  jce_state_redo(void)      { LOG_INFO(LOG_TAG, "redo (stub)"); }
bool  jce_state_can_undo(void)  { return false; }
bool  jce_state_can_redo(void)  { return false; }
