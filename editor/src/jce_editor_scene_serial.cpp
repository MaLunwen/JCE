/*
 * jce_editor_scene_serial.cpp  Scene JSON serialization and file I/O.
 *
 * Reads component data straight from the engine ECS (JceScene) — no editor
 * mirror store.  The scene JSON format is unchanged from the previous
 * mirror-based implementation so existing scenes stay loadable.
 */

#include "jce_editor_state_internal.h"
#include "jce_editor_file_util.h"

extern "C" {
#include <jce/graphics/jce_model.h>
}

#include <SDL3/SDL.h>
#include <cstring>
#include <cmath>
#include <filesystem>

/* ── Helpers for reading per-entity ECS components ───────────────── */

static void serialize_transform(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceTransform *t = jce_scene_get_transform(sc, e);
	if (!t) return;

	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "Transform");
	cJSON_AddNumberToObject(obj, "posX", t->position.x);
	cJSON_AddNumberToObject(obj, "posY", t->position.y);
	cJSON_AddNumberToObject(obj, "posZ", t->position.z);

	float euler[3];
	jce_q_to_euler_deg(t->rotation, euler);
	cJSON_AddNumberToObject(obj, "rotX", euler[0]);
	cJSON_AddNumberToObject(obj, "rotY", euler[1]);
	cJSON_AddNumberToObject(obj, "rotZ", euler[2]);

	cJSON_AddNumberToObject(obj, "scaleX", t->scale.x);
	cJSON_AddNumberToObject(obj, "scaleY", t->scale.y);
	cJSON_AddNumberToObject(obj, "scaleZ", t->scale.z);
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_mesh_renderer(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceMeshRenderer *mr = jce_scene_get_mesh_renderer(sc, e);
	if (!mr) return;

	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "MeshRenderer");
	cJSON_AddStringToObject(obj, "meshPath", mr->mesh_path);
	cJSON_AddStringToObject(obj, "materialPath", mr->material_path);
	cJSON_AddNumberToObject(obj, "meshShape", mr->mesh_shape);
	cJSON_AddNumberToObject(obj, "baseColorR", mr->base_color[0]);
	cJSON_AddNumberToObject(obj, "baseColorG", mr->base_color[1]);
	cJSON_AddNumberToObject(obj, "baseColorB", mr->base_color[2]);
	cJSON_AddNumberToObject(obj, "baseColorA", mr->base_color[3]);
	cJSON_AddNumberToObject(obj, "metallic",   mr->metallic);
	cJSON_AddNumberToObject(obj, "roughness",  mr->roughness);
	cJSON_AddNumberToObject(obj, "emissiveR",  mr->emissive[0]);
	cJSON_AddNumberToObject(obj, "emissiveG",  mr->emissive[1]);
	cJSON_AddNumberToObject(obj, "emissiveB",  mr->emissive[2]);
	cJSON_AddNumberToObject(obj, "normalScale", mr->normal_scale);
	cJSON_AddNumberToObject(obj, "aoStrength",  mr->ao_strength);
	cJSON_AddNumberToObject(obj, "alphaMode",   mr->alpha_mode);
	cJSON_AddNumberToObject(obj, "alphaCutoff", mr->alpha_cutoff);
	cJSON_AddBoolToObject(obj, "doubleSided", mr->double_sided);
	if (mr->albedo_tex[0])   cJSON_AddStringToObject(obj, "albedoTex",   mr->albedo_tex);
	if (mr->mr_tex[0])       cJSON_AddStringToObject(obj, "mrTex",       mr->mr_tex);
	if (mr->normal_tex[0])   cJSON_AddStringToObject(obj, "normalTex",   mr->normal_tex);
	if (mr->ao_tex[0])       cJSON_AddStringToObject(obj, "aoTex",       mr->ao_tex);
	if (mr->emissive_tex[0]) cJSON_AddStringToObject(obj, "emissiveTex", mr->emissive_tex);
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_camera(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceCameraComponent *c = jce_scene_get_camera(sc, e);
	if (!c) return;

	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "Camera");
	cJSON_AddNumberToObject(obj, "fov", c->fov_deg);
	cJSON_AddNumberToObject(obj, "nearClip", c->near_plane);
	cJSON_AddNumberToObject(obj, "farClip", c->far_plane);
	cJSON_AddBoolToObject(obj, "orthographic", c->ortho);
	cJSON_AddItemToArray(arr, obj);
}

static void add_unified_light_object(cJSON *arr,
                                     int   light_type,
                                     const jce_vec3 *color,
                                     float intensity,
                                     bool  has_radius, float radius,
                                     bool  has_cones, float inner_deg, float outer_deg,
                                     bool  casts_shadow)
{
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "Light");
	cJSON_AddNumberToObject(obj, "colorR", color->x);
	cJSON_AddNumberToObject(obj, "colorG", color->y);
	cJSON_AddNumberToObject(obj, "colorB", color->z);
	cJSON_AddNumberToObject(obj, "colorA", 1.0f);
	cJSON_AddNumberToObject(obj, "intensity", intensity);
	cJSON_AddNumberToObject(obj, "lightType", light_type);
	if (has_radius)
		cJSON_AddNumberToObject(obj, "radius", radius);
	if (has_cones) {
		cJSON_AddNumberToObject(obj, "innerConeDeg", inner_deg);
		cJSON_AddNumberToObject(obj, "outerConeDeg", outer_deg);
	}
	cJSON_AddBoolToObject(obj, "castsShadow", casts_shadow);
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_light(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceDirectionalLight *dl = jce_scene_get_dir_light(sc, e);
	if (dl) {
		add_unified_light_object(arr, 0, &dl->color, dl->intensity,
		                         false, 0.0f, false, 0.0f, 0.0f,
		                         dl->casts_shadow);
		return;
	}
	JcePointLight *pl = jce_scene_get_point_light(sc, e);
	if (pl) {
		add_unified_light_object(arr, 1, &pl->color, pl->intensity,
		                         true, pl->radius, false, 0.0f, 0.0f,
		                         false);
		return;
	}
	JceSpotLight *sl = jce_scene_get_spot_light(sc, e);
	if (sl) {
		float inner_deg = acosf(sl->inner_cone_cos) * JCE_RAD2DEG;
		float outer_deg = acosf(sl->outer_cone_cos) * JCE_RAD2DEG;
		add_unified_light_object(arr, 2, &sl->color, sl->intensity,
		                         true, sl->radius, true, inner_deg, outer_deg,
		                         false);
	}
}

static void serialize_skybox(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceSkyboxComponent *c = jce_scene_get_skybox(sc, e);
	if (!c) return;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "Skybox");
	cJSON_AddStringToObject(obj, "hdrPath", c->hdr_path);
	cJSON_AddNumberToObject(obj, "rotation", c->rotation);
	cJSON_AddNumberToObject(obj, "exposure", c->exposure);
	cJSON_AddBoolToObject(obj, "useAsIbl", c->use_as_ibl);
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_sprite_renderer(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceSpriteRendererComponent *c = jce_scene_get_sprite_renderer(sc, e);
	if (!c) return;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "SpriteRenderer");
	cJSON_AddStringToObject(obj, "spritePath", c->sprite_path);
	cJSON_AddNumberToObject(obj, "colorR", c->color[0]);
	cJSON_AddNumberToObject(obj, "colorG", c->color[1]);
	cJSON_AddNumberToObject(obj, "colorB", c->color[2]);
	cJSON_AddNumberToObject(obj, "colorA", c->color[3]);
	cJSON_AddBoolToObject(obj, "flipX", c->flip_x);
	cJSON_AddBoolToObject(obj, "flipY", c->flip_y);
	cJSON_AddNumberToObject(obj, "sortingOrder", c->sorting_order);
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_sprite_animator(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceSpriteAnimatorComponent *c = jce_scene_get_sprite_animator(sc, e);
	if (!c) return;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "SpriteAnimator");
	cJSON_AddStringToObject(obj, "sheetPath", c->sheet_path);
	cJSON_AddStringToObject(obj, "atlasPath", c->atlas_path);
	cJSON_AddNumberToObject(obj, "frameWidth", c->frame_width);
	cJSON_AddNumberToObject(obj, "frameHeight", c->frame_height);
	cJSON_AddStringToObject(obj, "currentAnim", c->current_anim);
	cJSON_AddNumberToObject(obj, "speed", c->speed);
	cJSON_AddBoolToObject(obj, "loop", c->loop);
	cJSON_AddBoolToObject(obj, "playing", c->playing);
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_animator(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceAnimatorComponent *c = jce_scene_get_animator(sc, e);
	if (!c) return;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "Animator");
	cJSON_AddStringToObject(obj, "clipName", c->clip_name);
	cJSON_AddNumberToObject(obj, "speed", c->speed);
	cJSON_AddBoolToObject(obj, "loop", c->loop);
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_skeletal_animator(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceSkeletalAnimatorComponent *c = jce_scene_get_skeletal_animator(sc, e);
	if (!c) return;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "SkeletalAnimator");
	cJSON_AddStringToObject(obj, "skeletonPath", c->skeleton_path);
	cJSON_AddNumberToObject(obj, "speed", c->speed);
	cJSON_AddBoolToObject(obj, "loop", c->loop);
	cJSON_AddNumberToObject(obj, "activeClip", c->active_clip);
	if (c->clip_count > 0) {
		cJSON *clips = cJSON_CreateArray();
		for (int ci = 0; ci < c->clip_count; ci++)
			cJSON_AddItemToArray(clips, cJSON_CreateString(c->clip_names[ci]));
		cJSON_AddItemToObject(obj, "clipNames", clips);
	}
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_constraint(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceConstraintComponent *c = jce_scene_get_constraint(sc, e);
	if (!c) return;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "Constraint");
	cJSON_AddNumberToObject(obj, "constraintType", c->constraint_type);
	cJSON_AddNumberToObject(obj, "targetEntity", (double)c->target_entity);
	cJSON_AddNumberToObject(obj, "pivotAx", c->pivot_a[0]);
	cJSON_AddNumberToObject(obj, "pivotAy", c->pivot_a[1]);
	cJSON_AddNumberToObject(obj, "pivotAz", c->pivot_a[2]);
	cJSON_AddNumberToObject(obj, "pivotBx", c->pivot_b[0]);
	cJSON_AddNumberToObject(obj, "pivotBy", c->pivot_b[1]);
	cJSON_AddNumberToObject(obj, "pivotBz", c->pivot_b[2]);
	cJSON_AddNumberToObject(obj, "axisX", c->axis[0]);
	cJSON_AddNumberToObject(obj, "axisY", c->axis[1]);
	cJSON_AddNumberToObject(obj, "axisZ", c->axis[2]);
	cJSON_AddNumberToObject(obj, "lowerLimit", c->lower_limit);
	cJSON_AddNumberToObject(obj, "upperLimit", c->upper_limit);
	cJSON_AddBoolToObject(obj, "disableCollision", c->disable_collision);
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_rigidbody(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceRigidBodyComponent *c = jce_scene_get_rigidbody(sc, e);
	if (!c) return;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "Rigidbody");
	cJSON_AddNumberToObject(obj, "mass", c->mass);
	cJSON_AddNumberToObject(obj, "drag", c->drag);
	cJSON_AddNumberToObject(obj, "angularDrag", c->angular_drag);
	cJSON_AddBoolToObject(obj, "useGravity", c->use_gravity);
	cJSON_AddBoolToObject(obj, "isKinematic", c->is_kinematic);
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_box_collider(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceBoxColliderComponent *c = jce_scene_get_box_collider(sc, e);
	if (!c) return;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "BoxCollider");
	cJSON_AddNumberToObject(obj, "centerX", c->center[0]);
	cJSON_AddNumberToObject(obj, "centerY", c->center[1]);
	cJSON_AddNumberToObject(obj, "centerZ", c->center[2]);
	cJSON_AddNumberToObject(obj, "sizeX", c->size[0]);
	cJSON_AddNumberToObject(obj, "sizeY", c->size[1]);
	cJSON_AddNumberToObject(obj, "sizeZ", c->size[2]);
	cJSON_AddBoolToObject(obj, "isTrigger", c->is_trigger);
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_sphere_collider(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceSphereColliderComponent *c = jce_scene_get_sphere_collider(sc, e);
	if (!c) return;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "SphereCollider");
	cJSON_AddNumberToObject(obj, "centerX", c->center[0]);
	cJSON_AddNumberToObject(obj, "centerY", c->center[1]);
	cJSON_AddNumberToObject(obj, "centerZ", c->center[2]);
	cJSON_AddNumberToObject(obj, "radius", c->radius);
	cJSON_AddBoolToObject(obj, "isTrigger", c->is_trigger);
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_character_controller(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceCharacterControllerComponent *c = jce_scene_get_character_controller(sc, e);
	if (!c) return;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "CharacterController");
	cJSON_AddNumberToObject(obj, "height", c->height);
	cJSON_AddNumberToObject(obj, "radius", c->radius);
	cJSON_AddNumberToObject(obj, "stepOffset", c->step_offset);
	cJSON_AddNumberToObject(obj, "slopeLimit", c->slope_limit);
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_audio_source(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceAudioSourceComponent *c = jce_scene_get_audio_source(sc, e);
	if (!c) return;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "AudioSource");
	cJSON_AddStringToObject(obj, "clipPath", c->clip_path);
	cJSON_AddNumberToObject(obj, "volume", c->volume);
	cJSON_AddNumberToObject(obj, "pitch", c->pitch);
	cJSON_AddNumberToObject(obj, "spatialBlend", c->spatial_blend);
	cJSON_AddBoolToObject(obj, "loop", c->loop);
	cJSON_AddBoolToObject(obj, "playOnAwake", c->play_on_awake);
	cJSON_AddItemToArray(arr, obj);
}

static void serialize_script(JceScene *sc, JceEntity e, cJSON *arr)
{
	JceScriptComponent *c = jce_scene_get_script(sc, e);
	if (!c) return;
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "type", "Script");
	cJSON_AddStringToObject(obj, "scriptPath", c->script_path);
	cJSON_AddItemToArray(arr, obj);
}

/* ── Build a "components" JSON array for an entity ───────────────── */

static cJSON *build_components_array(JceScene *sc, JceEntity e)
{
	cJSON *arr = cJSON_CreateArray();
	if (!arr) return NULL;

	uint32_t flags = jce_scene_get_component_flags(sc, e);

	if (flags & JCE_COMP_FLAG_TRANSFORM)            serialize_transform(sc, e, arr);
	if (flags & JCE_COMP_FLAG_MESH_RENDERER)        serialize_mesh_renderer(sc, e, arr);
	if (flags & JCE_COMP_FLAG_CAMERA)               serialize_camera(sc, e, arr);
	if (flags & (JCE_COMP_FLAG_DIR_LIGHT |
	             JCE_COMP_FLAG_POINT_LIGHT |
	             JCE_COMP_FLAG_SPOT_LIGHT))         serialize_light(sc, e, arr);
	if (flags & JCE_COMP_FLAG_SKYBOX)               serialize_skybox(sc, e, arr);
	if (flags & JCE_COMP_FLAG_SPRITE_RENDERER)      serialize_sprite_renderer(sc, e, arr);
	if (flags & JCE_COMP_FLAG_SPRITE_ANIMATOR)      serialize_sprite_animator(sc, e, arr);
	if (flags & JCE_COMP_FLAG_ANIMATOR)             serialize_animator(sc, e, arr);
	if (flags & JCE_COMP_FLAG_SKELETAL_ANIMATOR)    serialize_skeletal_animator(sc, e, arr);
	if (flags & JCE_COMP_FLAG_CONSTRAINT)           serialize_constraint(sc, e, arr);
	if (flags & JCE_COMP_FLAG_RIGIDBODY)            serialize_rigidbody(sc, e, arr);
	if (flags & JCE_COMP_FLAG_BOX_COLLIDER)         serialize_box_collider(sc, e, arr);
	if (flags & JCE_COMP_FLAG_SPHERE_COLLIDER)      serialize_sphere_collider(sc, e, arr);
	if (flags & JCE_COMP_FLAG_CHARACTER_CONTROLLER) serialize_character_controller(sc, e, arr);
	if (flags & JCE_COMP_FLAG_AUDIO_SOURCE)         serialize_audio_source(sc, e, arr);
	if (flags & JCE_COMP_FLAG_SCRIPT)               serialize_script(sc, e, arr);

	return arr;
}

/* ── Serialize entity tree to JSON (recursive) ───────────────────── */

cJSON *serialize_entity_tree_json(uint32_t entity_id)
{
	if (!s.scene || entity_id == 0) return NULL;
	JceEntity e = (JceEntity)entity_id;
	JceEditorMeta *meta = jce_scene_get_editor_meta(s.scene, e);
	if (!meta) return NULL;

	cJSON *node = cJSON_CreateObject();
	if (!node) return NULL;

	cJSON_AddStringToObject(node, "name", meta->name);
	cJSON_AddBoolToObject(node, "enabled", meta->enabled);
	cJSON_AddNumberToObject(node, "tagColor", (double)meta->tag_color);
	if (meta->tag[0] != '\0')
		cJSON_AddStringToObject(node, "tag", meta->tag);
	if (meta->prefab_instance) {
		cJSON_AddBoolToObject(node, "prefabInstance", true);
		if (meta->prefab_path[0] != '\0')
			cJSON_AddStringToObject(node, "prefabPath", meta->prefab_path);
	}

	cJSON *components = build_components_array(s.scene, e);
	if (!components) {
		cJSON_Delete(node);
		return NULL;
	}
	cJSON_AddItemToObject(node, "components", components);

	cJSON *children = cJSON_CreateArray();
	if (!children) {
		cJSON_Delete(node);
		return NULL;
	}
	cJSON_AddItemToObject(node, "children", children);

	JceEntity child_buf[JCE_MAX_CHILDREN];
	int cn = jce_scene_get_children(s.scene, e, child_buf, JCE_MAX_CHILDREN);
	for (int i = 0; i < cn; i++) {
		cJSON *child = serialize_entity_tree_json((uint32_t)child_buf[i]);
		if (child)
			cJSON_AddItemToArray(children, child);
	}

	return node;
}

/* ── Build full scene JSON root (flat entity array) ─────────────── */

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

	if (!s.scene)
		return root;

	for (uint32_t id : g_entity_order) {
		JceEntity e = (JceEntity)id;
		JceEditorMeta *meta = jce_scene_get_editor_meta(s.scene, e);
		if (!meta) continue;

		cJSON *eobj = cJSON_CreateObject();
		if (!eobj) continue;

		uint32_t parent_id = (uint32_t)jce_scene_get_parent(s.scene, e);

		cJSON_AddNumberToObject(eobj, "id", (double)id);
		cJSON_AddStringToObject(eobj, "name", meta->name);
		cJSON_AddNumberToObject(eobj, "parentId", (double)parent_id);
		cJSON_AddBoolToObject(eobj, "enabled", meta->enabled);
		if (meta->tag[0] != '\0')
			cJSON_AddStringToObject(eobj, "tag", meta->tag);
		cJSON_AddNumberToObject(eobj, "tagColor", (double)meta->tag_color);
		if (meta->prefab_instance) {
			cJSON_AddBoolToObject(eobj, "prefabInstance", true);
			if (meta->prefab_path[0] != '\0')
				cJSON_AddStringToObject(eobj, "prefabPath", meta->prefab_path);
		}

		cJSON *comps = build_components_array(s.scene, e);
		if (comps)
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
