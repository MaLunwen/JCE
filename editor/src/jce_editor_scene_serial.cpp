/*
 * jce_editor_scene_serial.cpp  Scene JSON serialization and file I/O.
 *
 * Handles writing scene/prefab data to JSON: component serialization,
 * entity tree serialization, scene file save/load, and prefab helpers.
 */

#include "jce_editor_state_internal.h"
#include "jce_editor_file_util.h"

/* ── Component type save name ────────────────────────────────────── */

static const char *component_type_save_name(JceComponentType type)
{
	switch (type) {
	case JCE_COMP_TRANSFORM:            return "Transform";
	case JCE_COMP_MESH_RENDERER:        return "MeshRenderer";
	case JCE_COMP_SPRITE_RENDERER:      return "SpriteRenderer";
	case JCE_COMP_CAMERA:               return "Camera";
	case JCE_COMP_LIGHT:                return "Light";
	case JCE_COMP_ANIMATOR:             return "Animator";
	case JCE_COMP_SKELETAL_ANIMATOR:    return "SkeletalAnimator";
	case JCE_COMP_RIGIDBODY:            return "Rigidbody";
	case JCE_COMP_BOX_COLLIDER:         return "BoxCollider";
	case JCE_COMP_SPHERE_COLLIDER:      return "SphereCollider";
	case JCE_COMP_CHARACTER_CONTROLLER: return "CharacterController";
	case JCE_COMP_AUDIO_SOURCE:         return "AudioSource";
	case JCE_COMP_SCRIPT:               return "Script";
	default:                            return "Unknown";
	}
}

/* ── Serialize a single component to JSON ────────────────────────── */

cJSON *serialize_component_json(const JceComponentInfo *comp)
{
	if (!comp) return NULL;

	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", component_type_save_name(comp->type));

	switch (comp->type) {
	case JCE_COMP_TRANSFORM:
		cJSON_AddNumberToObject(obj, "posX", comp->data.transform.pos[0]);
		cJSON_AddNumberToObject(obj, "posY", comp->data.transform.pos[1]);
		cJSON_AddNumberToObject(obj, "posZ", comp->data.transform.pos[2]);
		cJSON_AddNumberToObject(obj, "rotX", comp->data.transform.rot[0]);
		cJSON_AddNumberToObject(obj, "rotY", comp->data.transform.rot[1]);
		cJSON_AddNumberToObject(obj, "rotZ", comp->data.transform.rot[2]);
		cJSON_AddNumberToObject(obj, "scaleX", comp->data.transform.scale[0]);
		cJSON_AddNumberToObject(obj, "scaleY", comp->data.transform.scale[1]);
		cJSON_AddNumberToObject(obj, "scaleZ", comp->data.transform.scale[2]);
		break;
	case JCE_COMP_MESH_RENDERER:
		cJSON_AddStringToObject(obj, "meshPath", comp->data.mesh_renderer.mesh_path);
		cJSON_AddStringToObject(obj, "materialPath", comp->data.mesh_renderer.material_path);
		cJSON_AddNumberToObject(obj, "meshShape", comp->data.mesh_renderer.mesh_shape);
		/* PBR parameters. */
		cJSON_AddNumberToObject(obj, "baseColorR", comp->data.mesh_renderer.base_color[0]);
		cJSON_AddNumberToObject(obj, "baseColorG", comp->data.mesh_renderer.base_color[1]);
		cJSON_AddNumberToObject(obj, "baseColorB", comp->data.mesh_renderer.base_color[2]);
		cJSON_AddNumberToObject(obj, "baseColorA", comp->data.mesh_renderer.base_color[3]);
		cJSON_AddNumberToObject(obj, "metallic",   comp->data.mesh_renderer.metallic);
		cJSON_AddNumberToObject(obj, "roughness",  comp->data.mesh_renderer.roughness);
		cJSON_AddNumberToObject(obj, "emissiveR",  comp->data.mesh_renderer.emissive[0]);
		cJSON_AddNumberToObject(obj, "emissiveG",  comp->data.mesh_renderer.emissive[1]);
		cJSON_AddNumberToObject(obj, "emissiveB",  comp->data.mesh_renderer.emissive[2]);
		cJSON_AddNumberToObject(obj, "normalScale", comp->data.mesh_renderer.normal_scale);
		cJSON_AddNumberToObject(obj, "aoStrength",  comp->data.mesh_renderer.ao_strength);
		cJSON_AddNumberToObject(obj, "alphaMode",   comp->data.mesh_renderer.alpha_mode);
		cJSON_AddNumberToObject(obj, "alphaCutoff", comp->data.mesh_renderer.alpha_cutoff);
		cJSON_AddBoolToObject(obj, "doubleSided", comp->data.mesh_renderer.double_sided);
		if (comp->data.mesh_renderer.albedo_tex[0])
			cJSON_AddStringToObject(obj, "albedoTex", comp->data.mesh_renderer.albedo_tex);
		if (comp->data.mesh_renderer.mr_tex[0])
			cJSON_AddStringToObject(obj, "mrTex", comp->data.mesh_renderer.mr_tex);
		if (comp->data.mesh_renderer.normal_tex[0])
			cJSON_AddStringToObject(obj, "normalTex", comp->data.mesh_renderer.normal_tex);
		if (comp->data.mesh_renderer.ao_tex[0])
			cJSON_AddStringToObject(obj, "aoTex", comp->data.mesh_renderer.ao_tex);
		if (comp->data.mesh_renderer.emissive_tex[0])
			cJSON_AddStringToObject(obj, "emissiveTex", comp->data.mesh_renderer.emissive_tex);
		break;
	case JCE_COMP_SPRITE_RENDERER:
		cJSON_AddStringToObject(obj, "spritePath", comp->data.sprite_renderer.sprite_path);
		cJSON_AddNumberToObject(obj, "colorR", comp->data.sprite_renderer.color[0]);
		cJSON_AddNumberToObject(obj, "colorG", comp->data.sprite_renderer.color[1]);
		cJSON_AddNumberToObject(obj, "colorB", comp->data.sprite_renderer.color[2]);
		cJSON_AddNumberToObject(obj, "colorA", comp->data.sprite_renderer.color[3]);
		cJSON_AddBoolToObject(obj, "flipX", comp->data.sprite_renderer.flip_x);
		cJSON_AddBoolToObject(obj, "flipY", comp->data.sprite_renderer.flip_y);
		cJSON_AddNumberToObject(obj, "sortingOrder", comp->data.sprite_renderer.sorting_order);
		break;
	case JCE_COMP_ANIMATOR:
		cJSON_AddStringToObject(obj, "clipName", comp->data.animator.clip_name);
		cJSON_AddNumberToObject(obj, "speed", comp->data.animator.speed);
		cJSON_AddBoolToObject(obj, "loop", comp->data.animator.loop);
		break;
	case JCE_COMP_SKELETAL_ANIMATOR:
		cJSON_AddStringToObject(obj, "skeletonPath", comp->data.skeletal_animator.skeleton_path);
		cJSON_AddNumberToObject(obj, "speed", comp->data.skeletal_animator.speed);
		cJSON_AddBoolToObject(obj, "loop", comp->data.skeletal_animator.loop);
		cJSON_AddNumberToObject(obj, "activeClip", comp->data.skeletal_animator.active_clip);
		if (comp->data.skeletal_animator.clip_count > 0) {
			cJSON *clips = cJSON_CreateArray();
			for (int ci = 0; ci < comp->data.skeletal_animator.clip_count; ci++)
				cJSON_AddItemToArray(clips, cJSON_CreateString(comp->data.skeletal_animator.clip_names[ci]));
			cJSON_AddItemToObject(obj, "clipNames", clips);
		}
		break;
	case JCE_COMP_RIGIDBODY:
		cJSON_AddNumberToObject(obj, "mass", comp->data.rigidbody.mass);
		cJSON_AddNumberToObject(obj, "drag", comp->data.rigidbody.drag);
		cJSON_AddNumberToObject(obj, "angularDrag", comp->data.rigidbody.angular_drag);
		cJSON_AddBoolToObject(obj, "useGravity", comp->data.rigidbody.use_gravity);
		cJSON_AddBoolToObject(obj, "isKinematic", comp->data.rigidbody.is_kinematic);
		break;
	case JCE_COMP_BOX_COLLIDER:
		cJSON_AddNumberToObject(obj, "centerX", comp->data.box_collider.center[0]);
		cJSON_AddNumberToObject(obj, "centerY", comp->data.box_collider.center[1]);
		cJSON_AddNumberToObject(obj, "centerZ", comp->data.box_collider.center[2]);
		cJSON_AddNumberToObject(obj, "sizeX", comp->data.box_collider.size[0]);
		cJSON_AddNumberToObject(obj, "sizeY", comp->data.box_collider.size[1]);
		cJSON_AddNumberToObject(obj, "sizeZ", comp->data.box_collider.size[2]);
		cJSON_AddBoolToObject(obj, "isTrigger", comp->data.box_collider.is_trigger);
		break;
	case JCE_COMP_SPHERE_COLLIDER:
		cJSON_AddNumberToObject(obj, "centerX", comp->data.sphere_collider.center[0]);
		cJSON_AddNumberToObject(obj, "centerY", comp->data.sphere_collider.center[1]);
		cJSON_AddNumberToObject(obj, "centerZ", comp->data.sphere_collider.center[2]);
		cJSON_AddNumberToObject(obj, "radius", comp->data.sphere_collider.radius);
		cJSON_AddBoolToObject(obj, "isTrigger", comp->data.sphere_collider.is_trigger);
		break;
	case JCE_COMP_CHARACTER_CONTROLLER:
		cJSON_AddNumberToObject(obj, "height", comp->data.character_controller.height);
		cJSON_AddNumberToObject(obj, "radius", comp->data.character_controller.radius);
		cJSON_AddNumberToObject(obj, "stepOffset", comp->data.character_controller.step_offset);
		cJSON_AddNumberToObject(obj, "slopeLimit", comp->data.character_controller.slope_limit);
		break;
	case JCE_COMP_AUDIO_SOURCE:
		cJSON_AddStringToObject(obj, "clipPath", comp->data.audio_source.clip_path);
		cJSON_AddNumberToObject(obj, "volume", comp->data.audio_source.volume);
		cJSON_AddNumberToObject(obj, "pitch", comp->data.audio_source.pitch);
		cJSON_AddNumberToObject(obj, "spatialBlend", comp->data.audio_source.spatial_blend);
		cJSON_AddBoolToObject(obj, "loop", comp->data.audio_source.loop);
		cJSON_AddBoolToObject(obj, "playOnAwake", comp->data.audio_source.play_on_awake);
		break;
	case JCE_COMP_SCRIPT:
		cJSON_AddStringToObject(obj, "scriptPath", comp->data.script.script_path);
		break;
	case JCE_COMP_CAMERA:
		cJSON_AddNumberToObject(obj, "fov", comp->data.camera.fov);
		cJSON_AddNumberToObject(obj, "nearClip", comp->data.camera.near_clip);
		cJSON_AddNumberToObject(obj, "farClip", comp->data.camera.far_clip);
		cJSON_AddBoolToObject(obj, "orthographic", comp->data.camera.ortho);
		break;
	case JCE_COMP_LIGHT:
		cJSON_AddNumberToObject(obj, "colorR", comp->data.light.color[0]);
		cJSON_AddNumberToObject(obj, "colorG", comp->data.light.color[1]);
		cJSON_AddNumberToObject(obj, "colorB", comp->data.light.color[2]);
		cJSON_AddNumberToObject(obj, "colorA", comp->data.light.color[3]);
		cJSON_AddNumberToObject(obj, "intensity", comp->data.light.intensity);
		cJSON_AddNumberToObject(obj, "lightType", comp->data.light.type);
		break;
	default:
		break;
	}

	return obj;
}

/* ── Serialize entity tree to JSON (recursive) ───────────────────── */

cJSON *serialize_entity_tree_json(uint32_t entity_id)
{
	int idx = find_entity(entity_id);
	if (idx < 0)
		return NULL;

	JceEntityInfo *e = &s.entities[idx];
	cJSON *node = cJSON_CreateObject();
	if (!node)
		return NULL;

	cJSON_AddStringToObject(node, "name", e->name);
	cJSON_AddBoolToObject(node, "enabled", e->enabled);
	cJSON_AddNumberToObject(node, "tagColor", (double)e->tag_color);
	if (e->tag[0] != '\0')
		cJSON_AddStringToObject(node, "tag", e->tag);
	if (e->prefab_instance) {
		cJSON_AddBoolToObject(node, "prefabInstance", true);
		if (e->prefab_path[0] != '\0')
			cJSON_AddStringToObject(node, "prefabPath", e->prefab_path);
	}

	cJSON *components = cJSON_CreateArray();
	if (!components) {
		cJSON_Delete(node);
		return NULL;
	}
	cJSON_AddItemToObject(node, "components", components);
	for (int i = 0; i < e->component_count; i++) {
		cJSON *comp = serialize_component_json(&s.components[idx][i]);
		if (comp)
			cJSON_AddItemToArray(components, comp);
	}

	cJSON *children = cJSON_CreateArray();
	if (!children) {
		cJSON_Delete(node);
		return NULL;
	}
	cJSON_AddItemToObject(node, "children", children);
	for (int i = 0; i < e->child_count; i++) {
		cJSON *child = serialize_entity_tree_json(e->children[i]);
		if (child)
			cJSON_AddItemToArray(children, child);
	}

	return node;
}

/* ── Build full scene JSON root ──────────────────────────────────── */

cJSON *build_scene_json_root(void)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *contract = cJSON_CreateObject();
	cJSON *scene = cJSON_CreateObject();
	cJSON *entities = cJSON_CreateArray();
	if (!root || !contract || !scene || !entities) {
		cJSON_Delete(root);
		cJSON_Delete(contract);
		cJSON_Delete(scene);
		cJSON_Delete(entities);
		return NULL;
	}

	cJSON_AddItemToObject(root, JCE_SCENE_CONTRACT_KEY, contract);
	cJSON_AddStringToObject(contract, JCE_SCENE_CONTRACT_NAME_KEY,
	                        JCE_SCENE_CONTRACT_NAME);
	cJSON_AddNumberToObject(contract, JCE_SCENE_CONTRACT_MAJOR_KEY,
	                        JCE_SCENE_CONTRACT_MAJOR);
	cJSON_AddNumberToObject(contract, JCE_SCENE_CONTRACT_MINOR_KEY,
	                        JCE_SCENE_CONTRACT_MINOR);

	cJSON_AddItemToObject(root, JCE_SCENE_ROOT_KEY, scene);
	cJSON_AddNumberToObject(scene, JCE_SCENE_VERSION_KEY,
	                        JCE_SCENE_CONTRACT_MAJOR);
	cJSON_AddItemToObject(scene, JCE_SCENE_ENTITIES_KEY, entities);

	for (int i = 0; i < s.entity_count; i++) {
		JceEntityInfo *e = &s.entities[i];
		cJSON *eobj = cJSON_CreateObject();
		if (!eobj)
			continue;

		cJSON_AddNumberToObject(eobj, "id", (double)e->id);
		cJSON_AddStringToObject(eobj, "name", e->name);
		cJSON_AddNumberToObject(eobj, "parentId", (double)e->parent_id);
		cJSON_AddBoolToObject(eobj, "enabled", e->enabled);
		if (e->tag[0] != '\0')
			cJSON_AddStringToObject(eobj, "tag", e->tag);
		cJSON_AddNumberToObject(eobj, "tagColor", (double)e->tag_color);
		if (e->prefab_instance) {
			cJSON_AddBoolToObject(eobj, "prefabInstance", true);
			if (e->prefab_path[0] != '\0')
				cJSON_AddStringToObject(eobj, "prefabPath", e->prefab_path);
		}

		cJSON *comps = cJSON_CreateArray();
		for (int ci = 0; ci < e->component_count; ci++) {
			cJSON *cobj = serialize_component_json(&s.components[i][ci]);
			if (cobj)
				cJSON_AddItemToArray(comps, cobj);
		}
		cJSON_AddItemToObject(eobj, "components", comps);
		cJSON_AddItemToArray(entities, eobj);
	}

	return root;
}

/* ── Build prefab JSON root ──────────────────────────────────────── */

cJSON *build_prefab_json_root(uint32_t entity_id)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *contract = cJSON_CreateObject();
	cJSON *prefab = cJSON_CreateObject();
	cJSON *root_node = serialize_entity_tree_json(entity_id);
	if (!root || !contract || !prefab || !root_node) {
		cJSON_Delete(root);
		cJSON_Delete(contract);
		cJSON_Delete(prefab);
		cJSON_Delete(root_node);
		return NULL;
	}

	cJSON_AddItemToObject(root, JCE_SCENE_CONTRACT_KEY, contract);
	cJSON_AddStringToObject(contract, JCE_SCENE_CONTRACT_NAME_KEY,
	                        JCE_SCENE_CONTRACT_NAME);
	cJSON_AddNumberToObject(contract, JCE_SCENE_CONTRACT_MAJOR_KEY,
	                        JCE_SCENE_CONTRACT_MAJOR);
	cJSON_AddNumberToObject(contract, JCE_SCENE_CONTRACT_MINOR_KEY,
	                        JCE_SCENE_CONTRACT_MINOR);

	cJSON_AddItemToObject(root, "prefab", prefab);
	cJSON_AddNumberToObject(prefab, JCE_SCENE_VERSION_KEY,
	                        JCE_SCENE_CONTRACT_MAJOR);
	cJSON_AddItemToObject(prefab, "root", root_node);
	return root;
}

/* ── Find prefab root node in parsed JSON ────────────────────────── */

const cJSON *find_prefab_root_node(const cJSON *root)
{
	if (!root) return NULL;

	if (cJSON_IsObject(root)) {
		const cJSON *prefab = cJSON_GetObjectItemCaseSensitive(root, "prefab");
		if (cJSON_IsObject(prefab)) {
			const cJSON *node = cJSON_GetObjectItemCaseSensitive(prefab, "root");
			if (cJSON_IsObject(node))
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
	JceEntityInfo *e = jce_state_get_entity(entity_id);
	if (!e) return;

	if (prefab_path && prefab_path[0] != '\0') {
		e->prefab_instance = true;
		snprintf(e->prefab_path, sizeof(e->prefab_path), "%s", prefab_path);
	} else {
		e->prefab_instance = false;
		e->prefab_path[0] = '\0';
	}

	uint32_t child_ids[JCE_MAX_CHILDREN];
	int child_count = e->child_count;
	if (child_count > JCE_MAX_CHILDREN)
		child_count = JCE_MAX_CHILDREN;
	for (int i = 0; i < child_count; i++)
		child_ids[i] = e->children[i];

	for (int i = 0; i < child_count; i++)
		mark_prefab_instance_recursive(child_ids[i], prefab_path);
}

/* ── Scene file save ─────────────────────────────────────────────── */

bool jce_state_save_scene_file(const char *scene_path)
{
	if (!scene_path || scene_path[0] == '\0')
		return false;

	cJSON *root = build_scene_json_root();
	if (!root) {
		LOG_WARN(LOG_TAG, "scene save failed, JSON root creation error: %s", scene_path);
		return false;
	}

	if (!ed_write_json_to_file(scene_path, root)) {
		LOG_WARN(LOG_TAG, "scene save failed: %s", scene_path);
		return false;
	}

	update_scene_dir_from_path(scene_path);
	set_current_scene_path_internal(scene_path);
	s.scene_modified = false;
	LOG_INFO(LOG_TAG, "scene saved to %s (%d entities)", scene_path, s.entity_count);
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

	size_t file_size = 0;
	char *buf = (char *)ed_read_file(scene_path, &file_size);
	if (!buf) {
		LOG_WARN(LOG_TAG, "scene load failed, cannot read file: %s", scene_path);
		return false;
	}

	cJSON *root = cJSON_Parse(buf);
	ED_FREE(buf);

	if (!root) {
		LOG_WARN(LOG_TAG, "scene JSON parse failed: %s", scene_path);
		return false;
	}

	bool ok = false;
	{
		HistorySuspendScope suspend;
		ok = load_scene_from_parsed_root(root, scene_path, scene_path);
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
	cJSON_Delete(root);
	return ok;
}
