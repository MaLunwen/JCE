/*
 * jce_editor_prefab.cpp  Prefab lifecycle operations.
 *
 * Implements save, instantiate, revert, and query for prefab instances.
 */

#include "jce_editor_state_internal.h"
#include "jce_editor_file_util.h"

/* ── Save prefab ─────────────────────────────────────────────────── */

bool jce_state_save_prefab(uint32_t entity_id, const char *prefab_path)
{
	if (!prefab_path || prefab_path[0] == '\0')
		return false;
	if (find_entity(entity_id) < 0)
		return false;

	cJSON *root = build_prefab_json_root(entity_id);
	if (!root)
		return false;

	char *json_text = cJSON_Print(root);
	cJSON_Delete(root);
	if (!json_text)
		return false;

	FILE *fp = fopen(prefab_path, "wb");
	if (!fp) {
		cJSON_free(json_text);
		LOG_WARN(LOG_TAG, "prefab save failed, cannot open file: %s", prefab_path);
		return false;
	}

	size_t len = strlen(json_text);
	size_t wr = fwrite(json_text, 1, len, fp);
	fclose(fp);
	cJSON_free(json_text);
	if (wr != len) {
		LOG_WARN(LOG_TAG, "prefab save failed, short write: %s", prefab_path);
		return false;
	}

	HistoryEditScope edit_scope;
	mark_prefab_instance_recursive(entity_id, prefab_path);
	LOG_INFO(LOG_TAG, "prefab saved: %s", prefab_path);
	return true;
}

/* ── Instantiate prefab ──────────────────────────────────────────── */

uint32_t jce_state_instantiate_prefab(const char *prefab_path, uint32_t parent_id)
{
	if (!prefab_path || prefab_path[0] == '\0')
		return 0;

	size_t file_size = 0;
	char *buf = (char *)ed_read_file(prefab_path, &file_size);
	if (!buf) {
		LOG_WARN(LOG_TAG, "prefab instantiate failed, cannot read file: %s", prefab_path);
		return 0;
	}
	buf[file_size] = '\0';

	cJSON *root = cJSON_Parse(buf);
	ED_FREE(buf);
	if (!root) {
		LOG_WARN(LOG_TAG, "prefab instantiate failed, JSON parse error: %s", prefab_path);
		return 0;
	}

	int contract_major = (int)JCE_SCENE_CONTRACT_MAJOR;
	int contract_minor = (int)JCE_SCENE_CONTRACT_MINOR;
	parse_scene_contract_version(root, &contract_major, &contract_minor);
	(void)contract_minor;
	if (!jce_scene_contract_major_compatible((uint32_t)contract_major)) {
		LOG_WARN(LOG_TAG,
		         "prefab instantiate failed, unsupported contract major %d: %s",
		         contract_major, prefab_path);
		cJSON_Delete(root);
		return 0;
	}

	const cJSON *node = find_prefab_root_node(root);
	if (!node) {
		cJSON_Delete(root);
		LOG_WARN(LOG_TAG, "prefab instantiate failed, missing root node: %s", prefab_path);
		return 0;
	}

	HistoryEditScope edit_scope;
	uint32_t id = load_entity_tree_node(node, parent_id);
	cJSON_Delete(root);

	if (id != 0)
		mark_prefab_instance_recursive(id, prefab_path);

	return id;
}

/* ── Revert prefab instance ──────────────────────────────────────── */

bool jce_state_revert_prefab(uint32_t entity_id)
{
	JceEntityInfo *e = jce_state_get_entity(entity_id);
	if (!e || !e->prefab_instance || e->prefab_path[0] == '\0')
		return false;

	uint32_t parent_id = e->parent_id;
	bool was_selected = jce_state_is_selected(entity_id);
	char prefab_path[JCE_MAX_PREFAB_PATH];
	snprintf(prefab_path, sizeof(prefab_path), "%s", e->prefab_path);

	HistoryEditScope edit_scope;
	uint32_t new_id = jce_state_instantiate_prefab(prefab_path, parent_id);
	if (new_id == 0)
		return false;

	jce_state_delete_entity(entity_id);

	if (was_selected)
		jce_state_select_entity(new_id, false);
	return true;
}

/* ── Prefab queries ──────────────────────────────────────────────── */

bool jce_state_is_prefab_instance(uint32_t entity_id)
{
	JceEntityInfo *e = jce_state_get_entity(entity_id);
	return e ? e->prefab_instance : false;
}

const char *jce_state_get_prefab_path(uint32_t entity_id)
{
	JceEntityInfo *e = jce_state_get_entity(entity_id);
	if (!e || !e->prefab_instance || e->prefab_path[0] == '\0')
		return NULL;
	return e->prefab_path;
}
