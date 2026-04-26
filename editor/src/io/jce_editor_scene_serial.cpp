/*
 * jce_editor_scene_serial.cpp  Scene JSON serialization — thin wrapper.
 *
 * All component-level JSON serialization lives in the engine's
 * jce_scene_serial module.  This file handles editor-specific
 * concerns: prefab tree serialization, file I/O with dialogs,
 * undo/redo snapshot integration, mesh asset validation, and
 * round-trip verification.
 */

#include "jce_editor_file_util.h"
#include "jce_editor_state_internal.h"

extern "C" {
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/renderer/jce_model.h>
}

#include <SDL3/SDL.h>

#include <cmath>
#include <cstring>
#include <filesystem>

/* ── Serialize entity tree to JSON (recursive, for prefabs) ────────── */

JceJson *serialize_entity_tree_json(uint32_t entity_id)
{
	if (!s.scene || entity_id == 0) return NULL;
	JceEntity e = (JceEntity)entity_id;
	JceEditorMeta *meta = jce_scene_get_editor_meta(s.scene, e);
	if (!meta) return NULL;

	JceJson *node = jce_json_object();
	if (!node) return NULL;

	jce_json_set_string(node, "name", meta->name);
	jce_json_set_bool(node, "enabled", meta->enabled);
	jce_json_set_number(node, "tagColor", (double)meta->tag_color);
	if (meta->tag[0] != '\0')
		jce_json_set_string(node, "tag", meta->tag);
	if (meta->prefab_instance) {
		jce_json_set_bool(node, "prefabInstance", true);
		if (meta->prefab_path[0] != '\0')
			jce_json_set_string(node, "prefabPath", meta->prefab_path);
	}

	/* Delegate component serialization to the engine. */
	{
		JceJson *comps = jce_scene_serialize_entity_components(s.scene, e);
		if (comps)
			jce_json_set_child(node, "components", comps);
	}

	JceJson *children = jce_json_array();
	if (!children) {
		jce_json_free(node);
		return NULL;
	}
	jce_json_set_child(node, "children", children);

	JceEntity child_buf[JCE_MAX_CHILDREN];
	int cn = jce_scene_get_children(s.scene, e, child_buf, JCE_MAX_CHILDREN);
	for (int i = 0; i < cn; i++) {
		JceJson *child = serialize_entity_tree_json((uint32_t)child_buf[i]);
		if (child)
			jce_json_array_push(children, child);
	}

	return node;
}

/* ── Build full scene JSON root (delegates to engine) ──────────────── */

JceJson *build_scene_json_root(void)
{
	/* The engine serializer emits the contract envelope format. */
	return jce_scene_save_json(s.scene);
}

JceJson *build_prefab_json_root(uint32_t entity_id)
{
	JceJson *root = jce_json_object();
	JceJson *contract = jce_json_object();
	JceJson *prefab = jce_json_object();
	JceJson *root_node = serialize_entity_tree_json(entity_id);
	if (!root || !contract || !prefab || !root_node) {
		jce_json_free(root);
		jce_json_free(contract);
		jce_json_free(prefab);
		jce_json_free(root_node);
		return NULL;
	}

	jce_json_set_child(root, JCE_SCENE_CONTRACT_KEY, contract);
	jce_json_set_string(contract, JCE_SCENE_CONTRACT_NAME_KEY,
	                    JCE_SCENE_CONTRACT_NAME);
	jce_json_set_int(contract, JCE_SCENE_CONTRACT_MAJOR_KEY,
	                 JCE_SCENE_CONTRACT_MAJOR);
	jce_json_set_int(contract, JCE_SCENE_CONTRACT_MINOR_KEY,
	                 JCE_SCENE_CONTRACT_MINOR);

	jce_json_set_child(root, "prefab", prefab);
	jce_json_set_int(prefab, JCE_SCENE_VERSION_KEY,
	                 JCE_SCENE_CONTRACT_MAJOR);
	jce_json_set_child(prefab, "root", root_node);
	return root;
}

/* ── Find prefab root node in parsed JSON ────────────────────────── */

const JceJson *find_prefab_root_node(const JceJson *root)
{
	if (!root) return NULL;

	if (jce_json_is_object(root)) {
		const JceJson *prefab = jce_json_get(root, "prefab");
		if (jce_json_is_object(prefab)) {
			const JceJson *node = jce_json_get(prefab, "root");
			if (jce_json_is_object(node))
				return node;
		}
		if (looks_like_entity_object(root))
			return root;
	}

	return NULL;
}

/* ── Mark entity subtree as prefab instance ──────────────────────── */

void mark_prefab_instance_recursive(uint32_t entity_id, const char *prefab_path)
{
	if (!s.scene || entity_id == 0) return;
	JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)entity_id);
	if (!m) return;

	if (prefab_path && prefab_path[0] != '\0') {
		m->prefab_instance = true;
		snprintf(m->prefab_path, sizeof(m->prefab_path), "%s", prefab_path);
	} else {
		m->prefab_instance = false;
		m->prefab_path[0] = '\0';
	}

	uint32_t child_ids[JCE_MAX_CHILDREN];
	int child_count = jce_state_entity_children(entity_id, child_ids, JCE_MAX_CHILDREN);
	for (int i = 0; i < child_count; i++)
		mark_prefab_instance_recursive(child_ids[i], prefab_path);
}

/* ── Mesh asset validation (glTF/GLB via engine cgltf) ────────────── */

static bool is_gltf_extension(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (!dot) return false;
	return (strcmp(dot, ".gltf") == 0 || strcmp(dot, ".glb") == 0 ||
	        strcmp(dot, ".GLTF") == 0 || strcmp(dot, ".GLB") == 0);
}

struct MeshValidCtx { int checked; int failed; const char *scene_dir; };

static void validate_mesh_cb(JceScene *sc, JceEntity e, void *ud)
{
	auto *ctx = static_cast<MeshValidCtx *>(ud);

	JceMeshRenderer *mr = jce_scene_get_mesh_renderer(sc, e);
	if (!mr || mr->mesh_path[0] == '\0') return;
	if (!is_gltf_extension(mr->mesh_path)) return;

	namespace fs = std::filesystem;
	fs::path mesh_p(mr->mesh_path);
	if (!mesh_p.is_absolute() && ctx->scene_dir && ctx->scene_dir[0]) {
		mesh_p = fs::path(ctx->scene_dir) / mesh_p;
	}
	std::string abs_path = mesh_p.string();

	size_t file_size = 0;
	void *data = SDL_LoadFile(abs_path.c_str(), &file_size);
	if (!data) return;

	ctx->checked++;
	JceModel *model = jce_model_load_gltf_memory(data, (uint32_t)file_size,
	                                              mr->mesh_path);
	SDL_free(data);

	if (!model) {
		ctx->failed++;
		LOG_WARN(LOG_TAG, "engine cgltf cannot load mesh '%s' — "
		         "runtime may fail to display this model",
		         mr->mesh_path);
	} else {
		jce_model_destroy(model);
	}
}

static void validate_mesh_assets(const char *scene_path)
{
	namespace fs = std::filesystem;
	std::string scene_dir;
	if (scene_path) {
		fs::path sp(scene_path);
		if (sp.has_parent_path())
			scene_dir = sp.parent_path().string();
	}

	MeshValidCtx ctx = { 0, 0, scene_dir.c_str() };
	jce_scene_each_entity(s.scene, validate_mesh_cb, &ctx);

	if (ctx.checked > 0 && ctx.failed == 0) {
		LOG_SUCCESS(LOG_TAG, "mesh asset validation passed: %d glTF "
		            "files verified with engine cgltf", ctx.checked);
	} else if (ctx.failed > 0) {
		LOG_WARN(LOG_TAG, "mesh asset validation: %d/%d glTF files "
		         "failed engine cgltf load", ctx.failed, ctx.checked);
	}
}

/* ── Round-trip validation ────────────────────────────────────────── */

static void count_entity_cb(JceScene * /*s*/, JceEntity /*e*/, void *ud)
{
	(*(int *)ud)++;
}

static bool validate_scene_round_trip(const char *path)
{
	JceScene *verify = jce_scene_create();
	if (!verify) {
		LOG_ERROR(LOG_TAG, "round-trip validation: cannot create temp scene");
		return false;
	}

	bool ok = jce_scene_serial_load_file(verify, path);
	if (!ok) {
		LOG_ERROR(LOG_TAG, "round-trip validation FAILED: saved file cannot "
		          "be loaded back (%s)", path);
		jce_scene_destroy(verify);
		return false;
	}

	int loaded_count = 0;
	jce_scene_each_entity(verify, count_entity_cb, &loaded_count);

	int expected = (int)g_entity_order.size();
	if (loaded_count != expected) {
		LOG_WARN(LOG_TAG, "round-trip validation: entity count mismatch "
		         "(saved %d, loaded %d) in %s",
		         expected, loaded_count, path);
	} else {
		LOG_SUCCESS(LOG_TAG, "round-trip validation passed: %d entities (%s)",
		            loaded_count, path);
	}

	jce_scene_destroy(verify);
	return true;
}

/* ── Scene file save ─────────────────────────────────────────────── */

bool jce_state_save_scene_file(const char *scene_path)
{
	if (!scene_path || scene_path[0] == '\0')
		return false;

	if (!jce_scene_serial_save_file(s.scene, scene_path)) {
		LOG_WARN(LOG_TAG, "scene save failed: %s", scene_path);
		return false;
	}

	validate_scene_round_trip(scene_path);
	validate_mesh_assets(scene_path);

	update_scene_dir_from_path(scene_path);
	set_current_scene_path_internal(scene_path);
	s.scene_modified = false;
	LOG_INFO(LOG_TAG, "scene saved to %s (%d entities)",
	         scene_path, (int)g_entity_order.size());
	return true;
}

/* ── Scene path accessor ─────────────────────────────────────────── */

const char *jce_state_get_current_scene_path(void)
{
	return s.current_scene_path;
}

/* ── Scene file load ─────────────────────────────────────────────── */

bool jce_state_load_scene_file(const char *scene_path)
{
	if (!scene_path || scene_path[0] == '\0')
		return false;

	bool ok = false;
	{
		HistorySuspendScope suspend;

		clear_scene_entities();

		ok = jce_scene_serial_load_file(s.scene, scene_path);
		if (ok) {
			rebuild_entity_order_from_ecs();
			update_scene_dir_from_path(scene_path);
			set_current_scene_path_internal(scene_path);
			LOG_INFO(LOG_TAG, "scene loaded from %s (%d entities)",
			         scene_path, (int)g_entity_order.size());
		} else {
			LOG_WARN(LOG_TAG, "scene load failed: %s", scene_path);
		}
	}
	if (ok) {
		s_undo_history.clear();
		s_redo_history.clear();
		s.scene_modified = false;
		s_history_edit_nesting = 0;
		s_history_outer_edit_pushed_snapshot = false;
		s_history_manual_batch_depth = 0;
		s_transaction.active = false;
		s_transaction.label[0] = '\0';
		s_transaction.before.scene_json.clear();
		s_transaction.before.scene_path.clear();
	}
	return ok;
}
