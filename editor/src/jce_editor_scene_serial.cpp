/*
 * jce_editor_scene_serial.cpp  Scene JSON serialization and file I/O.
 *
 * Handles writing scene/prefab data to JSON: component serialization,
 * entity tree serialization, scene file save/load, and prefab helpers.
 */

#include "jce_editor_state_internal.h"
#include "jce_editor_ecs_adapter.h"
#include "jce_editor_file_util.h"

extern "C" {
#include <jce/graphics/jce_model.h>
}

#include <SDL3/SDL.h>
#include <cstring>
#include <filesystem>

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
	case JCE_COMP_SKYBOX:               return "Skybox";
	case JCE_COMP_SPRITE_ANIMATOR:      return "SpriteAnimator";
	case JCE_COMP_CONSTRAINT:           return "Constraint";
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
	case JCE_COMP_SKYBOX:
		cJSON_AddStringToObject(obj, "hdrPath", comp->data.skybox.hdr_path);
		cJSON_AddNumberToObject(obj, "rotation", comp->data.skybox.rotation);
		cJSON_AddNumberToObject(obj, "exposure", comp->data.skybox.exposure);
		cJSON_AddBoolToObject(obj, "useAsIbl", comp->data.skybox.use_as_ibl);
		break;
	case JCE_COMP_SPRITE_ANIMATOR:
		cJSON_AddStringToObject(obj, "sheetPath", comp->data.sprite_animator.sheet_path);
		cJSON_AddStringToObject(obj, "atlasPath", comp->data.sprite_animator.atlas_path);
		cJSON_AddNumberToObject(obj, "frameWidth", comp->data.sprite_animator.frame_width);
		cJSON_AddNumberToObject(obj, "frameHeight", comp->data.sprite_animator.frame_height);
		cJSON_AddStringToObject(obj, "currentAnim", comp->data.sprite_animator.current_anim);
		cJSON_AddNumberToObject(obj, "speed", comp->data.sprite_animator.speed);
		cJSON_AddBoolToObject(obj, "loop", comp->data.sprite_animator.loop);
		cJSON_AddBoolToObject(obj, "playing", comp->data.sprite_animator.playing);
		break;
	case JCE_COMP_CONSTRAINT:
		cJSON_AddNumberToObject(obj, "constraintType", comp->data.constraint.constraint_type);
		cJSON_AddNumberToObject(obj, "targetEntity", (double)comp->data.constraint.target_entity);
		cJSON_AddNumberToObject(obj, "pivotAx", comp->data.constraint.pivot_a[0]);
		cJSON_AddNumberToObject(obj, "pivotAy", comp->data.constraint.pivot_a[1]);
		cJSON_AddNumberToObject(obj, "pivotAz", comp->data.constraint.pivot_a[2]);
		cJSON_AddNumberToObject(obj, "pivotBx", comp->data.constraint.pivot_b[0]);
		cJSON_AddNumberToObject(obj, "pivotBy", comp->data.constraint.pivot_b[1]);
		cJSON_AddNumberToObject(obj, "pivotBz", comp->data.constraint.pivot_b[2]);
		cJSON_AddNumberToObject(obj, "axisX", comp->data.constraint.axis[0]);
		cJSON_AddNumberToObject(obj, "axisY", comp->data.constraint.axis[1]);
		cJSON_AddNumberToObject(obj, "axisZ", comp->data.constraint.axis[2]);
		cJSON_AddNumberToObject(obj, "lowerLimit", comp->data.constraint.lower_limit);
		cJSON_AddNumberToObject(obj, "upperLimit", comp->data.constraint.upper_limit);
		cJSON_AddBoolToObject(obj, "disableCollision", comp->data.constraint.disable_collision);
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
		if (comp->data.light.type == 1 || comp->data.light.type == 2)
			cJSON_AddNumberToObject(obj, "radius", comp->data.light.radius);
		if (comp->data.light.type == 2) {
			cJSON_AddNumberToObject(obj, "innerConeDeg", comp->data.light.inner_cone_deg);
			cJSON_AddNumberToObject(obj, "outerConeDeg", comp->data.light.outer_cone_deg);
		}
		cJSON_AddBoolToObject(obj, "castsShadow", comp->data.light.casts_shadow);
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

	/* Resolve relative path against scene directory. */
	namespace fs = std::filesystem;
	fs::path mesh_p(mr->mesh_path);
	if (!mesh_p.is_absolute() && ctx->scene_dir && ctx->scene_dir[0]) {
		mesh_p = fs::path(ctx->scene_dir) / mesh_p;
	}
	std::string abs_path = mesh_p.string();

	size_t file_size = 0;
	void *data = SDL_LoadFile(abs_path.c_str(), &file_size);
	if (!data) return; /* file not found — separate concern */

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

	/* Compare entity counts as a basic consistency check. */
	int loaded_count = 0;
	jce_scene_each_entity(verify, count_entity_cb, &loaded_count);

	if (loaded_count != s.entity_count) {
		LOG_WARN(LOG_TAG, "round-trip validation: entity count mismatch "
		         "(saved %d, loaded %d) in %s",
		         s.entity_count, loaded_count, path);
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

	/* ECS is kept in sync by CRUD operations, so we can serialize directly. */
	if (!jce_scene_serial_save_file(s.scene, scene_path)) {
		LOG_WARN(LOG_TAG, "scene save failed: %s", scene_path);
		return false;
	}

	/* Validate: reload the saved file into a temp scene to verify the
	   serialization round-trips correctly.  Logs a warning on mismatch. */
	validate_scene_round_trip(scene_path);

	/* Validate: check that any glTF/GLB mesh assets can be loaded by the
	   engine's cgltf loader (the same path the runtime uses). */
	validate_mesh_assets(scene_path);

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

	bool ok = false;
	{
		HistorySuspendScope suspend;

		/* Clear existing scene (destroys/recreates ECS world + editor arrays). */
		clear_scene_entities();

		/* Load via engine serializer → ECS. */
		ok = jce_scene_serial_load_file(s.scene, scene_path);
		if (ok) {
			/* Pull ECS → editor arrays. */
			jce_adapter_sync_ecs_to_editor();
			update_scene_dir_from_path(scene_path);
			set_current_scene_path_internal(scene_path);
			LOG_INFO(LOG_TAG, "scene loaded from %s (%d entities)",
			         scene_path, s.entity_count);
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
