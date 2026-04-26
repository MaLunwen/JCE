/*
 * jce_editor_scene_parse.cpp  Scene JSON parsing — thin wrapper.
 *
 * All component-level JSON parsing lives in the engine's
 * jce_scene_serial module.  This file handles editor-specific
 * concerns: entity creation through editor state, hierarchy tree
 * management (g_entity_order), contract version checks, and
 * format detection (tree vs flat vs object-map).
 */

#include "jce_editor_state_internal.h"

#include <cstring>

extern "C" {
#include <jce/middleware/scene/jce_scene_components_json.h>
}

/* ── JSON helpers (format detection only — no component parsing) ──── */

static const JceJson *json_get_any(const JceJson *obj, const char *const *keys, int key_count)
{
	if (!jce_json_is_object(obj)) return NULL;
	for (int i = 0; i < key_count; i++) {
		const JceJson *item = jce_json_get(obj, keys[i]);
		if (item) return item;
	}
	return NULL;
}

static const char *json_get_string_any(const JceJson *obj,
                                        const char *const *keys,
                                        int key_count)
{
	const JceJson *item = json_get_any(obj, keys, key_count);
	return jce_json_string_value(item, NULL);
}

/* ── Apply entity JSON fields via engine + editor metadata ────────── */

static void apply_entity_fields(uint32_t entity_id, const JceJson *obj)
{
	if (!jce_json_is_object(obj) || entity_id == 0 || !s.scene) return;
	JceEntity e = (JceEntity)entity_id;

	/* Engine parses all components and entity-level EditorMeta fields. */
	jce_scene_parse_entity_json(s.scene, e, obj);

	/* Ensure every entity has a Transform. */
	if (!jce_scene_has_transform(s.scene, e)) {
		JceTransform t;
		t.position = jce_v3(0.0f, 0.0f, 0.0f);
		t.rotation = jce_q_identity();
		t.scale    = jce_v3(1.0f, 1.0f, 1.0f);
		jce_scene_set_transform(s.scene, e, &t);
	}
}

/* ── Entity tree loading (for prefabs and tree-format scenes) ──────── */

uint32_t load_entity_tree_node(const JceJson *node, uint32_t parent_id)
{
	if (!jce_json_is_object(node)) return 0;

	static const char *const name_keys[] = { "name", "entityName", "label" };
	static const char *const children_keys[] = {
		"children", "nodes", "entities", "objects"
	};

	const char *name = json_get_string_any(node, name_keys,
		(int)(sizeof(name_keys) / sizeof(name_keys[0])));
	if (!name || name[0] == '\0')
		name = "Entity";

	uint32_t id = jce_state_create_entity(name, parent_id);
	apply_entity_fields(id, node);

	const JceJson *children = json_get_any(node, children_keys,
		(int)(sizeof(children_keys) / sizeof(children_keys[0])));
	if (jce_json_is_array(children)) {
		for (JceJson *child = jce_json_first_child(children); child;
		     child = jce_json_next_sibling(child))
			load_entity_tree_node(child, id);
	}

	return id;
}

/* ── Entity object detection ─────────────────────────────────────── */

bool looks_like_entity_object(const JceJson *obj)
{
	if (!jce_json_is_object(obj)) return false;

	static const char *const entity_keys[] = {
		"name", "entityName", "label", "children",
		"nodes", "entities", "objects", "parent",
		"parent_id", "parentId"
	};

	return json_get_any(obj, entity_keys,
		(int)(sizeof(entity_keys) / sizeof(entity_keys[0]))) != NULL;
}

/* ── Contract version parsing ────────────────────────────────────── */

bool parse_scene_contract_version(const JceJson *root, int *out_major, int *out_minor)
{
	if (out_major) *out_major = (int)JCE_SCENE_CONTRACT_MAJOR;
	if (out_minor) *out_minor = (int)JCE_SCENE_CONTRACT_MINOR;
	if (!jce_json_is_object(root)) return false;

	const JceJson *contract = jce_json_get(root, JCE_SCENE_CONTRACT_KEY);
	if (jce_json_is_object(contract)) {
		if (out_major)
			*out_major = jce_json_get_int(contract,
				JCE_SCENE_CONTRACT_MAJOR_KEY, *out_major);
		if (out_minor)
			*out_minor = jce_json_get_int(contract,
				JCE_SCENE_CONTRACT_MINOR_KEY, *out_minor);
		return true;
	}

	const JceJson *legacy_version = jce_json_get(root, JCE_SCENE_VERSION_KEY);
	if (jce_json_is_number(legacy_version)) {
		if (out_major) *out_major = (int)jce_json_number_value(legacy_version, *out_major);
		if (out_minor) *out_minor = 0;
		return true;
	}

	return false;
}

/* ── Load scene from parsed JSON root ────────────────────────────── */

bool load_scene_from_parsed_root(const JceJson *root,
                                 const char *scene_label,
                                 const char *scene_path)
{
	if (!root)
		return false;

	int contract_major = (int)JCE_SCENE_CONTRACT_MAJOR;
	int contract_minor = (int)JCE_SCENE_CONTRACT_MINOR;
	parse_scene_contract_version(root, &contract_major, &contract_minor);
	if (!jce_scene_contract_major_compatible((uint32_t)contract_major)) {
		LOG_WARN(LOG_TAG,
		         "scene load failed: unsupported contract major %d (expected %u)",
		         contract_major,
		         (unsigned)JCE_SCENE_CONTRACT_MAJOR);
		return false;
	}
	(void)contract_minor;

	/* Clear editor state and the ECS scene. */
	clear_scene_entities();

	/* Delegate all JSON parsing to the engine serializer. It handles
	 * contract envelopes, flat arrays, parent refs, key aliases, and
	 * every component type. */
	int n = jce_scene_load_json(s.scene, root);
	bool loaded_any = (n > 0);

	/* Sync editor entity order from what the engine created. */
	if (loaded_any)
		rebuild_entity_order_from_ecs();

	const char *label = (scene_label && scene_label[0] != '\0')
		? scene_label
		: "<memory>";
	if (!loaded_any)
		LOG_WARN(LOG_TAG, "scene load: no supported entity data in %s", label);

	jce_state_clear_selection();
	if (scene_path && scene_path[0] != '\0') {
		if (strcmp(s.current_scene_path, scene_path) != 0)
			update_scene_dir_from_path(scene_path);
		set_current_scene_path_internal(scene_path);
	} else {
		set_current_scene_path_internal(NULL);
	}

	LOG_INFO(LOG_TAG, "scene loaded from %s (%d entities)",
	         label, (int)g_entity_order.size());
	return true;
}
