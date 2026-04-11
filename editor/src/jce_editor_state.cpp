/*
 * jce_editor_state.cpp  Central editor state — core definitions.
 *
 * Defines the global editor state struct, selection, entity CRUD,
 * component management, mode accessors, and demo scene builder.
 * Parsing, serialization, history, play mode, and prefabs live in
 * their own jce_editor_*.cpp files.
 */

#include "jce_editor_state_internal.h"

/* Forward-declare only the one function we need from scene_render,
   avoiding a full include that creates a cpp-level circular dependency. */
extern "C" void jce_editor_scene_set_scene_dir(const char *dir);

/* ── Global State Definitions ─────────────────────────────────────── */

EditorInternalState s;

std::vector<EditorHistorySnapshot> s_undo_history;
std::vector<EditorHistorySnapshot> s_redo_history;
int  s_history_suspend_depth = 0;
int  s_history_edit_nesting = 0;
bool s_history_outer_edit_pushed_snapshot = false;
int  s_history_manual_batch_depth = 0;

EditorTransaction s_transaction;

/* ── Helper: find entity index by id ─────────────────────────────── */

int find_entity(uint32_t id)
{
	for (int i = 0; i < s.entity_count; i++)
		if (s.entities[i].id == id) return i;
	return -1;
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

	/* Find last path separator. */
	char *sep = strrchr(scene_dir, '/');
	char *bsep = strrchr(scene_dir, '\\');
	if (bsep && (!sep || bsep > sep)) sep = bsep;
	if (sep) {
		*sep = '\0';
		/* Go up one more level if we're in a "Scenes" subdirectory. */
		char *last_comp = strrchr(scene_dir, '/');
		char *last_bcomp = strrchr(scene_dir, '\\');
		if (last_bcomp && (!last_comp || last_bcomp > last_comp)) last_comp = last_bcomp;
		const char *dir_name = last_comp ? last_comp + 1 : scene_dir;
		if (_stricmp(dir_name, "Scenes") == 0 || _stricmp(dir_name, "scenes") == 0) {
			if (last_comp) *last_comp = '\0';
		}
	}

	jce_editor_scene_set_scene_dir(scene_dir);
}

void clear_scene_entities(void)
{
	/* Destroy and recreate engine scene to clear all ECS entities. */
	if (s.scene) {
		jce_scene_destroy(s.scene);
		s.scene = jce_scene_create();
	}
	s.entity_count = 0;
	s.next_id = 1;
	jce_state_clear_selection();
}

/* ── Demo scene builder ──────────────────────────────────────────── */

static void build_demo_scene(void)
{
	uint32_t root = jce_state_create_entity("Scene Root", 0);

	uint32_t cam = jce_state_create_entity("Main Camera", root);
	jce_state_add_component(cam, JCE_COMP_TRANSFORM);
	/* Set camera position. */
	{
		int idx = find_entity(cam);
		if (idx >= 0) {
			s.components[idx][0].data.transform.pos[1] = 5.0f;
			s.components[idx][0].data.transform.pos[2] = 10.0f;
		}
	}
	jce_state_add_component(cam, JCE_COMP_CAMERA);

	uint32_t lights = jce_state_create_entity("Lights", root);
	uint32_t dir_light = jce_state_create_entity("Directional Light", lights);
	jce_state_add_component(dir_light, JCE_COMP_TRANSFORM);
	jce_state_add_component(dir_light, JCE_COMP_LIGHT);

	uint32_t pt_light = jce_state_create_entity("Point Light", lights);
	jce_state_add_component(pt_light, JCE_COMP_TRANSFORM);
	jce_state_add_component(pt_light, JCE_COMP_LIGHT);
	{
		int idx = find_entity(pt_light);
		if (idx >= 0) {
			/* Point light type = 1. */
			for (int i = 0; i < s.entities[idx].component_count; i++) {
				if (s.components[idx][i].type == JCE_COMP_LIGHT)
					s.components[idx][i].data.light.type = 1;
			}
		}
	}

	uint32_t objs = jce_state_create_entity("Objects", root);
	uint32_t cube = jce_state_create_entity("Cube", objs);
	jce_state_add_component(cube, JCE_COMP_TRANSFORM);
	jce_state_add_component(cube, JCE_COMP_MESH_RENDERER);
	/* Position cube at (0, 0.5, 0) so it sits on the Y=0 ground. */
	{
		int idx = find_entity(cube);
		if (idx >= 0) {
			for (int i = 0; i < s.entities[idx].component_count; i++) {
				if (s.components[idx][i].type == JCE_COMP_TRANSFORM)
					s.components[idx][i].data.transform.pos[1] = 0.5f;
			}
		}
	}

	uint32_t sphere = jce_state_create_entity("Sphere", objs);
	jce_state_add_component(sphere, JCE_COMP_TRANSFORM);
	jce_state_add_component(sphere, JCE_COMP_MESH_RENDERER);
	/* Sphere at (2.5, 0.5, 0) — to the right of the cube. */
	{
		int idx = find_entity(sphere);
		if (idx >= 0) {
			for (int i = 0; i < s.entities[idx].component_count; i++) {
				if (s.components[idx][i].type == JCE_COMP_TRANSFORM) {
					s.components[idx][i].data.transform.pos[0] = 2.5f;
					s.components[idx][i].data.transform.pos[1] = 0.5f;
				}
				if (s.components[idx][i].type == JCE_COMP_MESH_RENDERER)
					s.components[idx][i].data.mesh_renderer.mesh_shape = JCE_MESH_SHAPE_SPHERE;
			}
		}
	}

	uint32_t plane = jce_state_create_entity("Plane", objs);
	jce_state_add_component(plane, JCE_COMP_TRANSFORM);
	jce_state_add_component(plane, JCE_COMP_MESH_RENDERER);
	/* Plane at (-2.5, 0, 0) — to the left, acts as a flat ground patch. */
	{
		int idx = find_entity(plane);
		if (idx >= 0) {
			for (int i = 0; i < s.entities[idx].component_count; i++) {
				if (s.components[idx][i].type == JCE_COMP_TRANSFORM) {
					s.components[idx][i].data.transform.pos[0] = -2.5f;
					s.components[idx][i].data.transform.scale[0] = 3.0f;
					s.components[idx][i].data.transform.scale[1] = 3.0f;
					s.components[idx][i].data.transform.scale[2] = 3.0f;
				}
				if (s.components[idx][i].type == JCE_COMP_MESH_RENDERER)
					s.components[idx][i].data.mesh_renderer.mesh_shape = JCE_MESH_SHAPE_PLANE;
			}
		}
	}

	uint32_t ui = jce_state_create_entity("UI", root);
	jce_state_create_entity("Canvas", ui);
}

/* ── Init / Shutdown ─────────────────────────────────────────────── */

void jce_editor_state_init(void)
{
	memset(&s, 0, sizeof(s));
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

	s.edit_mode   = JCE_EDIT_MODE_SELECT;
	s.gizmo_mode  = JCE_GIZMO_TRANSLATE;
	s.gizmo_space = JCE_GIZMO_LOCAL;
	s.view_mode   = JCE_VIEW_SHADED;
	s.play_state  = JCE_PLAY_STOPPED;
	s.show_grid   = true;
	s.current_scene_path[0] = '\0';

	s.scene = jce_scene_create();
	if (!s.scene) {
		LOG_ERROR(LOG_TAG, "failed to create engine scene");
	}

	{
		HistorySuspendScope suspend;
		clear_scene_entities();
		build_demo_scene();
	}

	s.initialized = true;
	LOG_INFO(LOG_TAG, "editor state initialized (%d demo entities)", s.entity_count);
}

void jce_editor_state_shutdown(void)
{
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

/* ── Entity Management ───────────────────────────────────────────── */

int jce_state_get_entity_count(void) { return s.entity_count; }

JceEntityInfo *jce_state_get_entity(uint32_t id)
{
	int idx = find_entity(id);
	return idx >= 0 ? &s.entities[idx] : NULL;
}

JceEntityInfo *jce_state_get_entity_by_index(int index)
{
	if (index < 0 || index >= s.entity_count)
		return NULL;
	return &s.entities[index];
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
	HistoryEditScope edit_scope;

	if (s.entity_count >= JCE_MAX_ENTITIES) return 0;

	JceEntityInfo *e = &s.entities[s.entity_count++];
	memset(e, 0, sizeof(*e));
	e->id = s.next_id++;
	snprintf(e->name, sizeof(e->name), "%s", name ? name : "Entity");
	e->enabled   = true;
	e->parent_id = parent_id;
	e->tag_color = JCE_TAG_NONE;
	e->prefab_instance = false;
	e->prefab_path[0] = '\0';

	/* Add to parent's children list. */
	if (parent_id != 0) {
		JceEntityInfo *parent = jce_state_get_entity(parent_id);
		if (parent && parent->child_count < JCE_MAX_CHILDREN)
			parent->children[parent->child_count++] = e->id;
	}

	/* Create in engine ECS. */
	if (s.scene) {
		e->ecs_entity = (uint64_t)jce_scene_create_entity(s.scene, name);
	}

	return e->id;
}

void jce_state_delete_entity(uint32_t id)
{
	HistoryEditScope edit_scope;

	int idx = find_entity(id);
	if (idx < 0) return;

	/* Remove from parent's children list. */
	JceEntityInfo *e = &s.entities[idx];

	/* Remove from engine ECS. */
	if (s.scene && e->ecs_entity != 0) {
		jce_scene_destroy_entity(s.scene, (JceEntity)e->ecs_entity);
	}

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
		int last = s.entity_count - 1;
		if (idx != last) {
			s.entities[idx] = s.entities[last];
			memcpy(s.components[idx], s.components[last],
			       sizeof(JceComponentInfo) * JCE_MAX_COMPONENTS);
		}
		s.entity_count--;
	}
}

void jce_state_rename_entity(uint32_t id, const char *name)
{
	HistoryEditScope edit_scope;

	JceEntityInfo *e = jce_state_get_entity(id);
	if (e) snprintf(e->name, sizeof(e->name), "%s", name);
}

void jce_state_set_entity_enabled(uint32_t id, bool enabled)
{
	HistoryEditScope edit_scope;

	JceEntityInfo *e = jce_state_get_entity(id);
	if (!e) return;

	e->enabled = enabled;
	for (int i = 0; i < e->child_count; i++)
		jce_state_set_entity_enabled(e->children[i], enabled);
}

void jce_state_set_entity_tag(uint32_t id, const char *tag)
{
	HistoryEditScope edit_scope;

	JceEntityInfo *e = jce_state_get_entity(id);
	if (e) snprintf(e->tag, sizeof(e->tag), "%s", tag ? tag : "");
}

void jce_state_set_entity_tag_color(uint32_t id, JceTagColor color)
{
	HistoryEditScope edit_scope;

	JceEntityInfo *e = jce_state_get_entity(id);
	if (e) e->tag_color = color;
}

void jce_state_reparent_entity(uint32_t id, uint32_t new_parent)
{
	HistoryEditScope edit_scope;

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
		if (new_p && new_p->child_count < JCE_MAX_CHILDREN)
			new_p->children[new_p->child_count++] = id;
	}
}

void jce_state_reorder_sibling(uint32_t entity_id, uint32_t ref_id,
                               bool insert_after)
{
	HistoryEditScope edit_scope;

	JceEntityInfo *e   = jce_state_get_entity(entity_id);
	JceEntityInfo *ref = jce_state_get_entity(ref_id);
	if (!e || !ref) return;
	if (entity_id == ref_id) return;

	/* Both must share the same parent. */
	if (e->parent_id != ref->parent_id) return;

	JceEntityInfo *parent = (e->parent_id != 0)
		? jce_state_get_entity(e->parent_id) : NULL;

	/* For root-level entities, we don't have a parent children array.
	   Rearranging root display order is handled by the flat entity list. */
	if (!parent) return;

	/* Remove entity_id from parent's children array. */
	int from = -1;
	for (int i = 0; i < parent->child_count; i++) {
		if (parent->children[i] == entity_id) { from = i; break; }
	}
	if (from < 0) return;

	/* Shift remaining children down to fill gap. */
	for (int i = from; i < parent->child_count - 1; i++)
		parent->children[i] = parent->children[i + 1];
	parent->child_count--;

	/* Find the ref_id position in the (now shorter) array. */
	int ref_pos = -1;
	for (int i = 0; i < parent->child_count; i++) {
		if (parent->children[i] == ref_id) { ref_pos = i; break; }
	}
	if (ref_pos < 0) {
		/* ref disappeared; just append back */
		parent->children[parent->child_count++] = entity_id;
		return;
	}

	int insert_pos = insert_after ? ref_pos + 1 : ref_pos;

	/* Shift children up to make room. */
	for (int i = parent->child_count; i > insert_pos; i--)
		parent->children[i] = parent->children[i - 1];
	parent->children[insert_pos] = entity_id;
	parent->child_count++;
}

uint32_t jce_state_duplicate_entity(uint32_t id)
{
	HistoryEditScope edit_scope;

	JceEntityInfo *src = jce_state_get_entity(id);
	if (!src) return 0;

	int src_idx = find_entity(id);
	char dup_name[JCE_MAX_ENTITY_NAME];
	snprintf(dup_name, sizeof(dup_name), "%s (Copy)", src->name);
	uint32_t dup = jce_state_create_entity(dup_name, src->parent_id);

	JceEntityInfo *d = jce_state_get_entity(dup);
	if (d) {
		d->tag_color = src->tag_color;
		d->enabled   = src->enabled;
		snprintf(d->tag, sizeof(d->tag), "%s", src->tag);
		d->prefab_instance = src->prefab_instance;
		snprintf(d->prefab_path, sizeof(d->prefab_path), "%s", src->prefab_path);
	}

	/* Copy components. */
	int dup_idx = find_entity(dup);
	if (src_idx >= 0 && dup_idx >= 0) {
		int count = s.entities[src_idx].component_count;
		if (count > JCE_MAX_COMPONENTS) count = JCE_MAX_COMPONENTS;
		memcpy(s.components[dup_idx], s.components[src_idx],
		       sizeof(JceComponentInfo) * count);
		s.entities[dup_idx].component_count = count;
	}

	return dup;
}

/* ── Components ──────────────────────────────────────────────────── */

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
	if (max < 1 || !out) return 0;

	int idx = find_entity(entity_id);
	if (idx < 0) return 0;

	JceEntityInfo *e = &s.entities[idx];
	int n = (e->component_count < max) ? e->component_count : max;
	for (int i = 0; i < n; i++)
		out[i] = s.components[idx][i];
	return n;
}

JceComponentInfo *jce_state_get_entity_components(uint32_t entity_id, int *out_count)
{
	int idx = find_entity(entity_id);
	if (idx < 0) {
		if (out_count) *out_count = 0;
		return NULL;
	}
	if (out_count) *out_count = s.entities[idx].component_count;
	return s.components[idx];
}

void jce_state_set_component(uint32_t entity_id, const JceComponentInfo *comp)
{
	HistoryEditScope edit_scope;

	if (!comp) return;
	int idx = find_entity(entity_id);
	if (idx < 0) return;

	JceEntityInfo *e = &s.entities[idx];
	/* Update existing component of same type. */
	bool found = false;
	for (int i = 0; i < e->component_count; i++) {
		if (s.components[idx][i].type == comp->type) {
			s.components[idx][i] = *comp;
			found = true;
			break;
		}
	}
	/* Not found — add it. */
	if (!found && e->component_count < JCE_MAX_COMPONENTS) {
		s.components[idx][e->component_count++] = *comp;
	}

	/* Sync transform to engine ECS. */
	if (comp->type == JCE_COMP_TRANSFORM && s.scene) {
		JceEntityInfo *ent = &s.entities[idx];
		if (ent->ecs_entity != 0) {
			JceTransform t;
			t.position = jce_v3(comp->data.transform.pos[0],
			                    comp->data.transform.pos[1],
			                    comp->data.transform.pos[2]);
			/* Convert Euler degrees to quaternion (engine API). */
			t.rotation = jce_euler_to_q(
				comp->data.transform.rot[0],
				comp->data.transform.rot[1],
				comp->data.transform.rot[2]);
			t.scale = jce_v3(comp->data.transform.scale[0],
			                 comp->data.transform.scale[1],
			                 comp->data.transform.scale[2]);
			jce_scene_set_transform(s.scene, (JceEntity)ent->ecs_entity, &t);
		}
	}
}

void jce_state_add_component(uint32_t entity_id, JceComponentType type)
{
	HistoryEditScope edit_scope;

	int idx = find_entity(entity_id);
	if (idx < 0) return;

	JceEntityInfo *e = &s.entities[idx];
	/* Check for duplicate. */
	for (int i = 0; i < e->component_count; i++) {
		if (s.components[idx][i].type == type)
			return;
	}
	if (e->component_count >= JCE_MAX_COMPONENTS) return;

	JceComponentInfo *c = &s.components[idx][e->component_count++];
	memset(c, 0, sizeof(*c));
	c->type = type;
	c->expanded = true;

	/* Set sensible defaults. */
	switch (type) {
	case JCE_COMP_TRANSFORM:
		c->data.transform.scale[0] = 1.0f;
		c->data.transform.scale[1] = 1.0f;
		c->data.transform.scale[2] = 1.0f;
		break;
	case JCE_COMP_MESH_RENDERER:
		c->data.mesh_renderer.base_color[0] = 1.0f;
		c->data.mesh_renderer.base_color[1] = 1.0f;
		c->data.mesh_renderer.base_color[2] = 1.0f;
		c->data.mesh_renderer.base_color[3] = 1.0f;
		c->data.mesh_renderer.roughness     = 0.5f;
		break;
	case JCE_COMP_CAMERA:
		c->data.camera.fov = 60.0f;
		c->data.camera.near_clip = 0.1f;
		c->data.camera.far_clip = 1000.0f;
		break;
	case JCE_COMP_LIGHT:
		c->data.light.color[0] = 1.0f;
		c->data.light.color[1] = 1.0f;
		c->data.light.color[2] = 1.0f;
		c->data.light.color[3] = 1.0f;
		c->data.light.intensity = 1.0f;
		break;
	default:
		break;
	}

	LOG_INFO(LOG_TAG, "add component %s to entity %u",
	         jce_component_type_name(type), entity_id);
}

void jce_state_remove_component(uint32_t entity_id, JceComponentType type)
{
	HistoryEditScope edit_scope;

	int idx = find_entity(entity_id);
	if (idx < 0) return;

	JceEntityInfo *e = &s.entities[idx];
	for (int i = 0; i < e->component_count; i++) {
		if (s.components[idx][i].type == type) {
			/* Swap-remove. */
			s.components[idx][i] = s.components[idx][--e->component_count];
			LOG_INFO(LOG_TAG, "remove component %s from entity %u",
			         jce_component_type_name(type), entity_id);
			return;
		}
	}
}

JceComponentType jce_component_type_from_name(const char *name)
{
	return component_type_from_name(name);
}

/* ── Mode Accessors ──────────────────────────────────────────────── */

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

bool  jce_state_get_2d_mode(void)            { return s.is_2d_mode; }
void  jce_state_set_2d_mode(bool is_2d)      { s.is_2d_mode = is_2d; }

bool  jce_state_get_live_preview(void)       { return s.live_preview; }
void  jce_state_set_live_preview(bool on)    { s.live_preview = on; }

void jce_state_set_scene(JceScene *scene) { s.scene = scene; }
JceScene *jce_state_get_scene(void) { return s.scene; }
