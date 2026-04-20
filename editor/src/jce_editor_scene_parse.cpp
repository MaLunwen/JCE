/*
 * jce_editor_scene_parse.cpp  Scene JSON parsing and entity loading.
 *
 * Handles all JSON deserialization of scene files: per-component parsing,
 * entity tree loading, flat-array loading with parent references,
 * object-map loading, and contract version checking.
 *
 * Components are written straight to the engine ECS — no editor mirror
 * store.  The accepted JSON format is unchanged from the previous
 * mirror-based implementation so existing scenes stay loadable.
 */

#include "jce_editor_state_internal.h"

#include <cstring>

/* ── Case-insensitive string comparison ──────────────────────────── */

static bool equals_ignore_case(const char *a, const char *b)
{
	if (!a || !b) return false;

	while (*a && *b) {
		char ca = *a;
		char cb = *b;
		if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
		if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
		if (ca != cb) return false;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

/* ── JSON helpers ─────────────────────────────────────────────────── */

static const cJSON *json_get_any(const cJSON *obj, const char *const *keys, int key_count)
{
	if (!cJSON_IsObject(obj)) return NULL;

	for (int i = 0; i < key_count; i++) {
		const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, keys[i]);
		if (item) return item;
	}
	return NULL;
}

static const char *json_get_string_any(const cJSON *obj,
                                       const char *const *keys,
                                       int key_count)
{
	const cJSON *item = json_get_any(obj, keys, key_count);
	return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool json_get_bool_any(const cJSON *obj,
                              const char *const *keys,
                              int key_count,
                              bool *out)
{
	const cJSON *item = json_get_any(obj, keys, key_count);
	if (!item || !out) return false;

	if (cJSON_IsBool(item)) {
		*out = cJSON_IsTrue(item);
		return true;
	}
	if (cJSON_IsNumber(item)) {
		*out = (item->valuedouble != 0.0);
		return true;
	}
	return false;
}

static JceTagColor parse_tag_color(const cJSON *obj)
{
	static const char *const color_keys[] = {
		"tag_color", "tagColor", "tag_color_id", "tagColorId"
	};
	const cJSON *item = json_get_any(obj, color_keys,
	                                 (int)(sizeof(color_keys) / sizeof(color_keys[0])));
	if (!item) return JCE_TAG_NONE;

	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		if (v >= JCE_TAG_NONE && v < JCE_TAG_COLOR_COUNT)
			return (JceTagColor)v;
		return JCE_TAG_NONE;
	}

	if (cJSON_IsString(item) && item->valuestring) {
		if (equals_ignore_case(item->valuestring, "red")) return JCE_TAG_RED;
		if (equals_ignore_case(item->valuestring, "orange")) return JCE_TAG_ORANGE;
		if (equals_ignore_case(item->valuestring, "yellow")) return JCE_TAG_YELLOW;
		if (equals_ignore_case(item->valuestring, "green")) return JCE_TAG_GREEN;
		if (equals_ignore_case(item->valuestring, "blue")) return JCE_TAG_BLUE;
		if (equals_ignore_case(item->valuestring, "purple")) return JCE_TAG_PURPLE;
		if (equals_ignore_case(item->valuestring, "gray")
		    || equals_ignore_case(item->valuestring, "grey"))
			return JCE_TAG_GRAY;
	}

	return JCE_TAG_NONE;
}

static std::string key_from_number(int value)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", value);
	return std::string(buf);
}

static std::string resolve_source_key(const cJSON *obj, int fallback)
{
	if (!cJSON_IsObject(obj))
		return key_from_number(fallback);

	const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(obj, "id");
	if (cJSON_IsString(id_item) && id_item->valuestring && id_item->valuestring[0] != '\0')
		return std::string(id_item->valuestring);
	if (cJSON_IsNumber(id_item))
		return key_from_number(id_item->valueint);

	return key_from_number(fallback);
}

static bool resolve_parent_key(const cJSON *obj, std::string *out_parent_key)
{
	if (!cJSON_IsObject(obj) || !out_parent_key) return false;

	static const char *const parent_keys[] = { "parent_id", "parentId", "parent" };
	const cJSON *item = json_get_any(obj, parent_keys,
		(int)(sizeof(parent_keys) / sizeof(parent_keys[0])));
	if (!item) return false;

	if (cJSON_IsNumber(item)) {
		if (item->valueint == 0) return false;
		*out_parent_key = key_from_number(item->valueint);
		return true;
	}

	if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
		if (strcmp(item->valuestring, "0") == 0) return false;
		*out_parent_key = item->valuestring;
		return true;
	}

	return false;
}

struct FlatEntityRef {
	std::string source_key;
	std::string source_alt_key;
	std::string parent_key;
	bool        has_parent;
	uint32_t    new_id;
};

/* ── Component name → flag (or special "Light" sentinel) ─────────── */

/* Sentinel used while parsing; the actual ECS component (DirLight /
   PointLight / SpotLight) is selected from the JSON "lightType" field. */
#define COMP_PARSE_LIGHT 0xFFFFFFFEu

static uint32_t component_flag_from_name(const char *name)
{
	if (!name) return 0;

	static const struct { const char *name; uint32_t flag; } map[] = {
		{ "Transform",            JCE_COMP_FLAG_TRANSFORM },
		{ "transform",            JCE_COMP_FLAG_TRANSFORM },
		{ "MeshRenderer",         JCE_COMP_FLAG_MESH_RENDERER },
		{ "Mesh Renderer",        JCE_COMP_FLAG_MESH_RENDERER },
		{ "meshRenderer",         JCE_COMP_FLAG_MESH_RENDERER },
		{ "mesh_renderer",        JCE_COMP_FLAG_MESH_RENDERER },
		{ "SpriteRenderer",       JCE_COMP_FLAG_SPRITE_RENDERER },
		{ "Sprite Renderer",      JCE_COMP_FLAG_SPRITE_RENDERER },
		{ "spriteRenderer",       JCE_COMP_FLAG_SPRITE_RENDERER },
		{ "sprite_renderer",      JCE_COMP_FLAG_SPRITE_RENDERER },
		{ "Camera",               JCE_COMP_FLAG_CAMERA },
		{ "camera",               JCE_COMP_FLAG_CAMERA },
		{ "Light",                COMP_PARSE_LIGHT },
		{ "light",                COMP_PARSE_LIGHT },
		{ "Animator",             JCE_COMP_FLAG_ANIMATOR },
		{ "animator",             JCE_COMP_FLAG_ANIMATOR },
		{ "SkeletalAnimator",     JCE_COMP_FLAG_SKELETAL_ANIMATOR },
		{ "Skeletal Animator",    JCE_COMP_FLAG_SKELETAL_ANIMATOR },
		{ "Rigidbody",            JCE_COMP_FLAG_RIGIDBODY },
		{ "rigidbody",            JCE_COMP_FLAG_RIGIDBODY },
		{ "BoxCollider",          JCE_COMP_FLAG_BOX_COLLIDER },
		{ "Box Collider",         JCE_COMP_FLAG_BOX_COLLIDER },
		{ "SphereCollider",       JCE_COMP_FLAG_SPHERE_COLLIDER },
		{ "Sphere Collider",      JCE_COMP_FLAG_SPHERE_COLLIDER },
		{ "CharacterController",  JCE_COMP_FLAG_CHARACTER_CONTROLLER },
		{ "Character Controller", JCE_COMP_FLAG_CHARACTER_CONTROLLER },
		{ "AudioSource",          JCE_COMP_FLAG_AUDIO_SOURCE },
		{ "Audio Source",         JCE_COMP_FLAG_AUDIO_SOURCE },
		{ "Script",               JCE_COMP_FLAG_SCRIPT },
		{ "script",               JCE_COMP_FLAG_SCRIPT },
		{ "Skybox",               JCE_COMP_FLAG_SKYBOX },
		{ "skybox",               JCE_COMP_FLAG_SKYBOX },
		{ "SpriteAnimator",       JCE_COMP_FLAG_SPRITE_ANIMATOR },
		{ "Sprite Animator",      JCE_COMP_FLAG_SPRITE_ANIMATOR },
		{ "spriteAnimator",       JCE_COMP_FLAG_SPRITE_ANIMATOR },
		{ "Constraint",           JCE_COMP_FLAG_CONSTRAINT },
		{ "constraint",           JCE_COMP_FLAG_CONSTRAINT },
	};

	for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
		if (strcmp(name, map[i].name) == 0)
			return map[i].flag;
	}
	return 0;
}

/* ── JSON Number / String Helpers ────────────────────────────────── */

static float json_get_float(const cJSON *obj, const char *key, float def)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	return cJSON_IsNumber(item) ? (float)item->valuedouble : def;
}

static float json_get_float_any2(const cJSON *obj, const char *k1, const char *k2, float def)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, k1);
	if (!cJSON_IsNumber(item))
		item = cJSON_GetObjectItemCaseSensitive(obj, k2);
	return cJSON_IsNumber(item) ? (float)item->valuedouble : def;
}

static const char *json_get_string(const cJSON *obj, const char *key)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL;
}

/* ── Per-component JSON parsers (write directly to ECS) ──────────── */

static void parse_json_transform(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceTransform t;
	memset(&t, 0, sizeof(t));

	t.position.x = json_get_float_any2(props, "posX", "pos_x", 0.0f);
	t.position.y = json_get_float_any2(props, "posY", "pos_y", 0.0f);
	t.position.z = json_get_float_any2(props, "posZ", "pos_z", 0.0f);

	float rx = json_get_float_any2(props, "rotX", "rot_x", 0.0f);
	float ry = json_get_float_any2(props, "rotY", "rot_y", 0.0f);
	float rz = json_get_float_any2(props, "rotZ", "rot_z", 0.0f);
	const cJSON *rw = cJSON_GetObjectItemCaseSensitive(props, "rotW");
	if (!rw) rw = cJSON_GetObjectItemCaseSensitive(props, "rot_w");
	if (cJSON_IsNumber(rw)) {
		jce_quat q = {{ rx, ry, rz, (float)rw->valuedouble }};
		t.rotation = jce_q_normalize(q);
	} else {
		float euler[3] = { rx, ry, rz };
		t.rotation = jce_q_from_euler_deg(euler);
	}

	t.scale.x = json_get_float_any2(props, "scaleX", "scale_x", 1.0f);
	t.scale.y = json_get_float_any2(props, "scaleY", "scale_y", 1.0f);
	t.scale.z = json_get_float_any2(props, "scaleZ", "scale_z", 1.0f);

	jce_scene_set_transform(scene, e, &t);
}

static void parse_json_camera(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceCameraComponent c;
	memset(&c, 0, sizeof(c));
	c.fov_deg    = json_get_float(props, "fov", 60.0f);
	c.near_plane = json_get_float_any2(props, "nearClip", "near_clip", 0.1f);
	c.far_plane  = json_get_float_any2(props, "farClip", "far_clip", 1000.0f);
	bool ortho = false;
	static const char *const ortho_keys[] = { "orthographic", "ortho", "isOrtho" };
	json_get_bool_any(props, ortho_keys,
		(int)(sizeof(ortho_keys) / sizeof(ortho_keys[0])), &ortho);
	c.ortho = ortho;
	c.is_primary = true;
	jce_scene_set_camera(scene, e, &c);
}

static void parse_json_light(const cJSON *props, JceScene *scene, JceEntity e)
{
	float colorR = json_get_float_any2(props, "colorR", "color_r", 1.0f);
	float colorG = json_get_float_any2(props, "colorG", "color_g", 1.0f);
	float colorB = json_get_float_any2(props, "colorB", "color_b", 1.0f);
	float intensity = json_get_float(props, "intensity", 1.0f);
	bool casts_shadow = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "castsShadow"));

	int type = 0;
	const cJSON *lt = cJSON_GetObjectItemCaseSensitive(props, "lightType");
	if (!lt) lt = cJSON_GetObjectItemCaseSensitive(props, "type");
	if (cJSON_IsNumber(lt))
		type = lt->valueint;
	else if (cJSON_IsString(lt) && lt->valuestring) {
		if (equals_ignore_case(lt->valuestring, "point")) type = 1;
		else if (equals_ignore_case(lt->valuestring, "spot")) type = 2;
	}

	if (type == 1) {
		JcePointLight pl;
		memset(&pl, 0, sizeof(pl));
		pl.color.x = colorR;
		pl.color.y = colorG;
		pl.color.z = colorB;
		pl.intensity = intensity;
		pl.radius    = json_get_float(props, "radius", 10.0f);
		jce_scene_set_point_light(scene, e, &pl);
	} else if (type == 2) {
		JceSpotLight sl;
		memset(&sl, 0, sizeof(sl));
		sl.color.x = colorR;
		sl.color.y = colorG;
		sl.color.z = colorB;
		sl.intensity = intensity;
		sl.radius    = json_get_float(props, "radius", 10.0f);
		float inner_deg = json_get_float_any2(props, "innerConeDeg", "inner_cone_deg", 25.0f);
		float outer_deg = json_get_float_any2(props, "outerConeDeg", "outer_cone_deg", 35.0f);
		sl.inner_cone_cos = cosf(inner_deg * JCE_DEG2RAD);
		sl.outer_cone_cos = cosf(outer_deg * JCE_DEG2RAD);
		jce_scene_set_spot_light(scene, e, &sl);
	} else {
		JceDirectionalLight dl;
		memset(&dl, 0, sizeof(dl));
		dl.color.x = colorR;
		dl.color.y = colorG;
		dl.color.z = colorB;
		dl.intensity    = intensity;
		dl.casts_shadow = casts_shadow;
		jce_scene_set_dir_light(scene, e, &dl);
	}
}

static void parse_json_mesh_renderer(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceMeshRenderer mr;
	memset(&mr, 0, sizeof(mr));

	static const char *const mesh_keys[] = { "meshPath", "mesh_path", "mesh" };
	static const char *const mat_keys[] = { "materialPath", "material_path", "material" };
	const char *mesh = json_get_string_any(props, mesh_keys,
		(int)(sizeof(mesh_keys) / sizeof(mesh_keys[0])));
	const char *mat = json_get_string_any(props, mat_keys,
		(int)(sizeof(mat_keys) / sizeof(mat_keys[0])));
	if (mesh)
		snprintf(mr.mesh_path, sizeof(mr.mesh_path), "%s", mesh);
	if (mat)
		snprintf(mr.material_path, sizeof(mr.material_path), "%s", mat);
	mr.mesh_shape = (int)json_get_float(props, "meshShape", 0.0f);

	mr.base_color[0] = json_get_float(props, "baseColorR", 1.0f);
	mr.base_color[1] = json_get_float(props, "baseColorG", 1.0f);
	mr.base_color[2] = json_get_float(props, "baseColorB", 1.0f);
	mr.base_color[3] = json_get_float(props, "baseColorA", 1.0f);
	mr.metallic      = json_get_float(props, "metallic", 0.0f);
	mr.roughness     = json_get_float(props, "roughness", 1.0f);
	mr.emissive[0]   = json_get_float(props, "emissiveR", 0.0f);
	mr.emissive[1]   = json_get_float(props, "emissiveG", 0.0f);
	mr.emissive[2]   = json_get_float(props, "emissiveB", 0.0f);
	mr.normal_scale  = json_get_float(props, "normalScale", 1.0f);
	mr.ao_strength   = json_get_float(props, "aoStrength", 1.0f);
	mr.alpha_mode    = (int)json_get_float(props, "alphaMode", 0.0f);
	mr.alpha_cutoff  = json_get_float(props, "alphaCutoff", 0.5f);
	mr.double_sided  = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "doubleSided"));
	mr.visible       = true;

	{ const char *v = json_get_string(props, "albedoTex");
	  if (v) snprintf(mr.albedo_tex, sizeof(mr.albedo_tex), "%s", v); }
	{ const char *v = json_get_string(props, "mrTex");
	  if (v) snprintf(mr.mr_tex, sizeof(mr.mr_tex), "%s", v); }
	{ const char *v = json_get_string(props, "normalTex");
	  if (v) snprintf(mr.normal_tex, sizeof(mr.normal_tex), "%s", v); }
	{ const char *v = json_get_string(props, "aoTex");
	  if (v) snprintf(mr.ao_tex, sizeof(mr.ao_tex), "%s", v); }
	{ const char *v = json_get_string(props, "emissiveTex");
	  if (v) snprintf(mr.emissive_tex, sizeof(mr.emissive_tex), "%s", v); }

	jce_scene_set_mesh_renderer(scene, e, &mr);
}

static void parse_json_sprite_renderer(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceSpriteRendererComponent c;
	memset(&c, 0, sizeof(c));
	{ const char *v = json_get_string(props, "spritePath");
	  if (v) snprintf(c.sprite_path, sizeof(c.sprite_path), "%s", v); }
	c.color[0] = json_get_float(props, "colorR", 1.0f);
	c.color[1] = json_get_float(props, "colorG", 1.0f);
	c.color[2] = json_get_float(props, "colorB", 1.0f);
	c.color[3] = json_get_float(props, "colorA", 1.0f);
	c.flip_x = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "flipX"));
	c.flip_y = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "flipY"));
	c.sorting_order = (int)json_get_float(props, "sortingOrder", 0.0f);
	jce_scene_set_sprite_renderer(scene, e, &c);
}

static void parse_json_animator(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceAnimatorComponent c;
	memset(&c, 0, sizeof(c));
	{ const char *v = json_get_string(props, "clipName");
	  if (v) snprintf(c.clip_name, sizeof(c.clip_name), "%s", v); }
	c.speed   = json_get_float(props, "speed", 1.0f);
	c.loop    = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "loop"));
	c.playing = false;
	jce_scene_set_animator(scene, e, &c);
}

static void parse_json_skeletal_animator(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceSkeletalAnimatorComponent c;
	memset(&c, 0, sizeof(c));
	{ const char *v = json_get_string(props, "skeletonPath");
	  if (v) snprintf(c.skeleton_path, sizeof(c.skeleton_path), "%s", v); }
	c.speed       = json_get_float(props, "speed", 1.0f);
	c.loop        = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "loop"));
	c.playing     = false;
	c.active_clip = (int)json_get_float(props, "activeClip", 0.0f);

	const cJSON *clips = cJSON_GetObjectItemCaseSensitive(props, "clipNames");
	c.clip_count = 0;
	if (cJSON_IsArray(clips)) {
		int n = cJSON_GetArraySize(clips);
		if (n > 8) n = 8;
		for (int ci = 0; ci < n; ci++) {
			const cJSON *ce = cJSON_GetArrayItem(clips, ci);
			if (cJSON_IsString(ce) && ce->valuestring)
				snprintf(c.clip_names[ci], 64, "%s", ce->valuestring);
		}
		c.clip_count = n;
	}
	jce_scene_set_skeletal_animator(scene, e, &c);
}

static void parse_json_rigidbody(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceRigidBodyComponent c;
	memset(&c, 0, sizeof(c));
	c.mass         = json_get_float(props, "mass", 1.0f);
	c.drag         = json_get_float(props, "drag", 0.0f);
	c.angular_drag = json_get_float(props, "angularDrag", 0.05f);
	c.use_gravity  = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "useGravity"));
	c.is_kinematic = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "isKinematic"));
	if (!cJSON_GetObjectItemCaseSensitive(props, "useGravity"))
		c.use_gravity = true;
	jce_scene_set_rigidbody(scene, e, &c);
}

static void parse_json_box_collider(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceBoxColliderComponent c;
	memset(&c, 0, sizeof(c));
	c.center[0] = json_get_float(props, "centerX", 0.0f);
	c.center[1] = json_get_float(props, "centerY", 0.0f);
	c.center[2] = json_get_float(props, "centerZ", 0.0f);
	c.size[0]   = json_get_float(props, "sizeX", 1.0f);
	c.size[1]   = json_get_float(props, "sizeY", 1.0f);
	c.size[2]   = json_get_float(props, "sizeZ", 1.0f);
	c.is_trigger = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "isTrigger"));
	jce_scene_set_box_collider(scene, e, &c);
}

static void parse_json_sphere_collider(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceSphereColliderComponent c;
	memset(&c, 0, sizeof(c));
	c.center[0] = json_get_float(props, "centerX", 0.0f);
	c.center[1] = json_get_float(props, "centerY", 0.0f);
	c.center[2] = json_get_float(props, "centerZ", 0.0f);
	c.radius     = json_get_float(props, "radius", 0.5f);
	c.is_trigger = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "isTrigger"));
	jce_scene_set_sphere_collider(scene, e, &c);
}

static void parse_json_character_controller(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceCharacterControllerComponent c;
	memset(&c, 0, sizeof(c));
	c.height      = json_get_float(props, "height", 2.0f);
	c.radius      = json_get_float(props, "radius", 0.5f);
	c.step_offset = json_get_float(props, "stepOffset", 0.3f);
	c.slope_limit = json_get_float(props, "slopeLimit", 45.0f);
	jce_scene_set_character_controller(scene, e, &c);
}

static void parse_json_audio_source(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceAudioSourceComponent c;
	memset(&c, 0, sizeof(c));
	{ const char *v = json_get_string(props, "clipPath");
	  if (v) snprintf(c.clip_path, sizeof(c.clip_path), "%s", v); }
	c.volume        = json_get_float(props, "volume", 1.0f);
	c.pitch         = json_get_float(props, "pitch", 1.0f);
	c.spatial_blend = json_get_float(props, "spatialBlend", 0.0f);
	c.loop          = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "loop"));
	c.play_on_awake = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "playOnAwake"));
	if (!cJSON_GetObjectItemCaseSensitive(props, "playOnAwake"))
		c.play_on_awake = true;
	jce_scene_set_audio_source(scene, e, &c);
}

static void parse_json_script(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceScriptComponent c;
	memset(&c, 0, sizeof(c));
	{ const char *v = json_get_string(props, "scriptPath");
	  if (v) snprintf(c.script_path, sizeof(c.script_path), "%s", v); }
	jce_scene_set_script(scene, e, &c);
}

static void parse_json_skybox(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceSkyboxComponent c;
	memset(&c, 0, sizeof(c));
	{ const char *v = json_get_string(props, "hdrPath");
	  if (v) snprintf(c.hdr_path, sizeof(c.hdr_path), "%s", v); }
	c.rotation   = json_get_float(props, "rotation", 0.0f);
	c.exposure   = json_get_float(props, "exposure", 1.0f);
	c.use_as_ibl = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "useAsIbl"));
	jce_scene_set_skybox(scene, e, &c);
}

static void parse_json_sprite_animator(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceSpriteAnimatorComponent c;
	memset(&c, 0, sizeof(c));
	{ const char *v = json_get_string(props, "sheetPath");
	  if (v) snprintf(c.sheet_path, sizeof(c.sheet_path), "%s", v); }
	{ const char *v = json_get_string(props, "atlasPath");
	  if (v) snprintf(c.atlas_path, sizeof(c.atlas_path), "%s", v); }
	c.frame_width  = (int)json_get_float(props, "frameWidth", 64.0f);
	c.frame_height = (int)json_get_float(props, "frameHeight", 64.0f);
	{ const char *v = json_get_string(props, "currentAnim");
	  if (v) snprintf(c.current_anim, sizeof(c.current_anim), "%s", v); }
	c.speed   = json_get_float(props, "speed", 1.0f);
	c.loop    = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "loop"));
	c.playing = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "playing"));
	jce_scene_set_sprite_animator(scene, e, &c);
}

static void parse_json_constraint(const cJSON *props, JceScene *scene, JceEntity e)
{
	JceConstraintComponent c;
	memset(&c, 0, sizeof(c));
	c.constraint_type = (int)json_get_float(props, "constraintType", 0.0f);
	c.target_entity   = (uint32_t)json_get_float(props, "targetEntity", 0.0f);
	c.pivot_a[0] = json_get_float(props, "pivotAx", 0.0f);
	c.pivot_a[1] = json_get_float(props, "pivotAy", 0.0f);
	c.pivot_a[2] = json_get_float(props, "pivotAz", 0.0f);
	c.pivot_b[0] = json_get_float(props, "pivotBx", 0.0f);
	c.pivot_b[1] = json_get_float(props, "pivotBy", 0.0f);
	c.pivot_b[2] = json_get_float(props, "pivotBz", 0.0f);
	c.axis[0] = json_get_float(props, "axisX", 0.0f);
	c.axis[1] = json_get_float(props, "axisY", 1.0f);
	c.axis[2] = json_get_float(props, "axisZ", 0.0f);
	c.lower_limit = json_get_float(props, "lowerLimit", 0.0f);
	c.upper_limit = json_get_float(props, "upperLimit", 0.0f);
	c.disable_collision = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "disableCollision"));
	jce_scene_set_constraint(scene, e, &c);
}

/* ── Parse a single component JSON object → ECS write ────────────── */

static bool parse_component_json(const cJSON *comp_json, JceScene *scene, JceEntity e)
{
	if (!cJSON_IsObject(comp_json) || !scene || e == JCE_ENTITY_INVALID)
		return false;

	static const char *const type_keys[] = { "type", "componentType", "class" };
	const char *type_str = json_get_string_any(comp_json, type_keys,
		(int)(sizeof(type_keys) / sizeof(type_keys[0])));
	if (!type_str) return false;

	uint32_t flag = component_flag_from_name(type_str);
	if (flag == 0) return false;

	const cJSON *props = cJSON_GetObjectItemCaseSensitive(comp_json, "properties");
	if (!cJSON_IsObject(props))
		props = comp_json;

	switch (flag) {
	case JCE_COMP_FLAG_TRANSFORM:            parse_json_transform(props, scene, e);            break;
	case JCE_COMP_FLAG_CAMERA:               parse_json_camera(props, scene, e);               break;
	case COMP_PARSE_LIGHT:                   parse_json_light(props, scene, e);                break;
	case JCE_COMP_FLAG_MESH_RENDERER:        parse_json_mesh_renderer(props, scene, e);        break;
	case JCE_COMP_FLAG_SPRITE_RENDERER:      parse_json_sprite_renderer(props, scene, e);      break;
	case JCE_COMP_FLAG_ANIMATOR:             parse_json_animator(props, scene, e);             break;
	case JCE_COMP_FLAG_SKELETAL_ANIMATOR:    parse_json_skeletal_animator(props, scene, e);    break;
	case JCE_COMP_FLAG_RIGIDBODY:            parse_json_rigidbody(props, scene, e);            break;
	case JCE_COMP_FLAG_BOX_COLLIDER:         parse_json_box_collider(props, scene, e);         break;
	case JCE_COMP_FLAG_SPHERE_COLLIDER:      parse_json_sphere_collider(props, scene, e);      break;
	case JCE_COMP_FLAG_CHARACTER_CONTROLLER: parse_json_character_controller(props, scene, e); break;
	case JCE_COMP_FLAG_AUDIO_SOURCE:         parse_json_audio_source(props, scene, e);         break;
	case JCE_COMP_FLAG_SCRIPT:               parse_json_script(props, scene, e);               break;
	case JCE_COMP_FLAG_SKYBOX:               parse_json_skybox(props, scene, e);               break;
	case JCE_COMP_FLAG_SPRITE_ANIMATOR:      parse_json_sprite_animator(props, scene, e);      break;
	case JCE_COMP_FLAG_CONSTRAINT:           parse_json_constraint(props, scene, e);           break;
	default: return false;
	}

	return true;
}

/* ── Parse Components Array for an Entity ────────────────────────── */

static void parse_entity_components(const cJSON *obj, JceScene *scene, JceEntity e)
{
	if (!cJSON_IsObject(obj) || !scene || e == JCE_ENTITY_INVALID) return;

	static const char *const comp_keys[] = { "components", "component" };
	const cJSON *comps = json_get_any(obj, comp_keys,
		(int)(sizeof(comp_keys) / sizeof(comp_keys[0])));
	if (!cJSON_IsArray(comps)) return;

	for (cJSON *c = comps->child; c; c = c->next)
		parse_component_json(c, scene, e);
}

/* ── Ensure Entity has a Transform Component ─────────────────────── */

static void ensure_transform_component(JceScene *scene, JceEntity e)
{
	if (!scene || e == JCE_ENTITY_INVALID) return;
	if (jce_scene_has_transform(scene, e)) return;

	JceTransform t;
	t.position = jce_v3(0.0f, 0.0f, 0.0f);
	t.rotation = jce_q_identity();
	t.scale    = jce_v3(1.0f, 1.0f, 1.0f);
	jce_scene_set_transform(scene, e, &t);
}

/* ── Flat entity ref helpers ─────────────────────────────────────── */

static uint32_t find_new_id_for_key(const std::vector<FlatEntityRef> &refs,
                                    const std::string &key)
{
	for (size_t i = 0; i < refs.size(); i++) {
		if (refs[i].source_key == key)
			return refs[i].new_id;
		if (!refs[i].source_alt_key.empty() && refs[i].source_alt_key == key)
			return refs[i].new_id;
	}
	return 0;
}

/* ── Apply per-entity fields to ECS EditorMeta + parse components ── */

static void apply_entity_fields(uint32_t entity_id, const cJSON *obj)
{
	if (!cJSON_IsObject(obj) || entity_id == 0 || !s.scene) return;

	JceEntity e = (JceEntity)entity_id;
	JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, e);
	if (!m) return;

	static const char *const name_keys[] = { "name", "entityName", "label" };
	static const char *const tag_keys[] = { "tag", "entityTag" };
	static const char *const enabled_keys[] = { "enabled", "isEnabled", "active" };
	static const char *const prefab_path_keys[] = { "prefabPath", "prefab_path" };
	static const char *const prefab_instance_keys[] = {
		"prefabInstance", "prefab_instance", "isPrefabInstance"
	};

	const char *name = json_get_string_any(obj, name_keys,
		(int)(sizeof(name_keys) / sizeof(name_keys[0])));
	if (name && name[0] != '\0')
		snprintf(m->name, sizeof(m->name), "%s", name);

	const char *tag = json_get_string_any(obj, tag_keys,
		(int)(sizeof(tag_keys) / sizeof(tag_keys[0])));
	if (tag)
		snprintf(m->tag, sizeof(m->tag), "%s", tag);

	bool enabled = true;
	if (json_get_bool_any(obj, enabled_keys,
		(int)(sizeof(enabled_keys) / sizeof(enabled_keys[0])),
		&enabled))
		m->enabled = enabled;

	bool prefab_instance = m->prefab_instance;
	if (json_get_bool_any(obj, prefab_instance_keys,
		(int)(sizeof(prefab_instance_keys) / sizeof(prefab_instance_keys[0])),
		&prefab_instance)) {
		m->prefab_instance = prefab_instance;
		if (!prefab_instance)
			m->prefab_path[0] = '\0';
	}

	const char *prefab_path = json_get_string_any(obj, prefab_path_keys,
		(int)(sizeof(prefab_path_keys) / sizeof(prefab_path_keys[0])));
	if (prefab_path && prefab_path[0] != '\0') {
		snprintf(m->prefab_path, sizeof(m->prefab_path), "%s", prefab_path);
		m->prefab_instance = true;
	}

	m->tag_color = (uint8_t)parse_tag_color(obj);

	parse_entity_components(obj, s.scene, e);
	ensure_transform_component(s.scene, e);
}

/* ── Entity tree loading ─────────────────────────────────────────── */

uint32_t load_entity_tree_node(const cJSON *node, uint32_t parent_id)
{
	if (!cJSON_IsObject(node)) return 0;

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

	const cJSON *children = json_get_any(node, children_keys,
		(int)(sizeof(children_keys) / sizeof(children_keys[0])));
	if (cJSON_IsArray(children)) {
		for (cJSON *child = children->child; child; child = child->next)
			load_entity_tree_node(child, id);
	}

	return id;
}

static bool array_has_parent_refs(const cJSON *arr)
{
	if (!cJSON_IsArray(arr)) return false;

	for (cJSON *item = arr->child; item; item = item->next) {
		if (!cJSON_IsObject(item)) continue;
		const cJSON *p1 = cJSON_GetObjectItemCaseSensitive(item, "parent_id");
		const cJSON *p2 = cJSON_GetObjectItemCaseSensitive(item, "parentId");
		const cJSON *p3 = cJSON_GetObjectItemCaseSensitive(item, "parent");
		if (cJSON_IsNumber(p1) || cJSON_IsNumber(p2))
			return true;
		if (cJSON_IsString(p3) && p3->valuestring && p3->valuestring[0] != '\0'
		    && strcmp(p3->valuestring, "0") != 0)
			return true;
	}
	return false;
}

static void load_entities_from_array(const cJSON *arr)
{
	if (!cJSON_IsArray(arr)) return;

	if (!array_has_parent_refs(arr)) {
		for (cJSON *item = arr->child; item; item = item->next)
			load_entity_tree_node(item, 0);
		return;
	}

	std::vector<FlatEntityRef> refs;
	refs.reserve((size_t)cJSON_GetArraySize(arr));

	int fallback_id = 1;
	for (cJSON *item = arr->child; item; item = item->next) {
		if (!cJSON_IsObject(item)) continue;

		static const char *const name_keys[] = { "name", "entityName", "label" };
		const char *name = json_get_string_any(item, name_keys,
			(int)(sizeof(name_keys) / sizeof(name_keys[0])));
		if (!name || name[0] == '\0')
			name = "Entity";

		uint32_t id = jce_state_create_entity(name, 0);
		apply_entity_fields(id, item);

		FlatEntityRef ref;
		ref.source_key = resolve_source_key(item, fallback_id++);
		ref.source_alt_key.clear();
		ref.has_parent = resolve_parent_key(item, &ref.parent_key);
		ref.new_id = id;
		refs.push_back(ref);
	}

	for (size_t i = 0; i < refs.size(); i++) {
		if (!refs[i].has_parent) continue;

		uint32_t parent_new_id = find_new_id_for_key(refs, refs[i].parent_key);

		if (parent_new_id != 0 && parent_new_id != refs[i].new_id)
			jce_state_reparent_entity(refs[i].new_id, parent_new_id);
	}
}

static bool object_map_has_parent_refs(const cJSON *obj)
{
	if (!cJSON_IsObject(obj)) return false;

	for (const cJSON *item = obj->child; item; item = item->next) {
		if (!cJSON_IsObject(item)) continue;
		std::string parent_key;
		if (resolve_parent_key(item, &parent_key))
			return true;
	}
	return false;
}

static void load_entities_from_object_map(const cJSON *obj)
{
	if (!cJSON_IsObject(obj)) return;

	if (!object_map_has_parent_refs(obj)) {
		for (const cJSON *item = obj->child; item; item = item->next) {
			if (cJSON_IsObject(item))
				load_entity_tree_node(item, 0);
		}
		return;
	}

	std::vector<FlatEntityRef> refs;
	int fallback_id = 1;

	for (const cJSON *item = obj->child; item; item = item->next) {
		if (!cJSON_IsObject(item)) continue;

		static const char *const name_keys[] = { "name", "entityName", "label" };
		const char *name = json_get_string_any(item, name_keys,
			(int)(sizeof(name_keys) / sizeof(name_keys[0])));
		if (!name || name[0] == '\0')
			name = "Entity";

		uint32_t id = jce_state_create_entity(name, 0);
		apply_entity_fields(id, item);

		FlatEntityRef ref;
		std::string resolved = resolve_source_key(item, fallback_id++);
		if (item->string && item->string[0] != '\0') {
			ref.source_key = item->string;
			ref.source_alt_key = (ref.source_key == resolved) ? std::string() : resolved;
		} else {
			ref.source_key = resolved;
			ref.source_alt_key.clear();
		}
		ref.has_parent = resolve_parent_key(item, &ref.parent_key);
		ref.new_id = id;
		refs.push_back(ref);
	}

	for (size_t i = 0; i < refs.size(); i++) {
		if (!refs[i].has_parent) continue;

		uint32_t parent_new_id = find_new_id_for_key(refs, refs[i].parent_key);
		if (parent_new_id != 0 && parent_new_id != refs[i].new_id)
			jce_state_reparent_entity(refs[i].new_id, parent_new_id);
	}
}

/* ── Entity object detection ─────────────────────────────────────── */

bool looks_like_entity_object(const cJSON *obj)
{
	if (!cJSON_IsObject(obj)) return false;

	static const char *const entity_keys[] = {
		"name", "entityName", "label", "children",
		"nodes", "entities", "objects", "parent",
		"parent_id", "parentId"
	};

	return json_get_any(obj, entity_keys,
		(int)(sizeof(entity_keys) / sizeof(entity_keys[0]))) != NULL;
}

/* ── Contract version parsing ────────────────────────────────────── */

bool parse_scene_contract_version(const cJSON *root, int *out_major, int *out_minor)
{
	if (out_major) *out_major = (int)JCE_SCENE_CONTRACT_MAJOR;
	if (out_minor) *out_minor = (int)JCE_SCENE_CONTRACT_MINOR;
	if (!cJSON_IsObject(root)) return false;

	const cJSON *contract = cJSON_GetObjectItemCaseSensitive(root,
		JCE_SCENE_CONTRACT_KEY);
	if (cJSON_IsObject(contract)) {
		const cJSON *major_item = cJSON_GetObjectItemCaseSensitive(contract,
			JCE_SCENE_CONTRACT_MAJOR_KEY);
		const cJSON *minor_item = cJSON_GetObjectItemCaseSensitive(contract,
			JCE_SCENE_CONTRACT_MINOR_KEY);

		if (cJSON_IsNumber(major_item) && out_major)
			*out_major = major_item->valueint;
		if (cJSON_IsNumber(minor_item) && out_minor)
			*out_minor = minor_item->valueint;
		return true;
	}

	const cJSON *legacy_version = cJSON_GetObjectItemCaseSensitive(root,
		JCE_SCENE_VERSION_KEY);
	if (cJSON_IsNumber(legacy_version)) {
		if (out_major) *out_major = legacy_version->valueint;
		if (out_minor) *out_minor = 0;
		return true;
	}

	return false;
}

/* ── Load scene from parsed JSON root ────────────────────────────── */

bool load_scene_from_parsed_root(const cJSON *root,
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

	const cJSON *container = root;
	if (cJSON_IsObject(root)) {
		const cJSON *scene = cJSON_GetObjectItemCaseSensitive(root,
			JCE_SCENE_ROOT_KEY);
		if (cJSON_IsObject(scene))
			container = scene;
	}

	clear_scene_entities();
	bool loaded_any = false;

	if (cJSON_IsArray(container)) {
		int before = (int)g_entity_order.size();
		load_entities_from_array(container);
		loaded_any = ((int)g_entity_order.size() > before);
	} else if (cJSON_IsObject(container)) {
		static const char *const array_keys[] = {
			JCE_SCENE_ENTITIES_KEY, "objects", "nodes", "children"
		};
		const cJSON *entities_data = json_get_any(container, array_keys,
			(int)(sizeof(array_keys) / sizeof(array_keys[0])));
		if (cJSON_IsArray(entities_data)) {
			int before = (int)g_entity_order.size();
			load_entities_from_array(entities_data);
			loaded_any = loaded_any || ((int)g_entity_order.size() > before);
		} else if (cJSON_IsObject(entities_data)) {
			int before = (int)g_entity_order.size();
			load_entities_from_object_map(entities_data);
			loaded_any = loaded_any || ((int)g_entity_order.size() > before);
		}

		static const char *const root_node_keys[] = {
			"root", "sceneRoot", "hierarchyRoot"
		};
		const cJSON *root_node = json_get_any(container, root_node_keys,
			(int)(sizeof(root_node_keys) / sizeof(root_node_keys[0])));
		if (cJSON_IsObject(root_node)) {
			int before = (int)g_entity_order.size();
			load_entity_tree_node(root_node, 0);
			loaded_any = loaded_any || ((int)g_entity_order.size() > before);
		}

		if (!loaded_any && looks_like_entity_object(container)) {
			int before = (int)g_entity_order.size();
			load_entity_tree_node(container, 0);
			loaded_any = loaded_any || ((int)g_entity_order.size() > before);
		}
	}

	const char *label = (scene_label && scene_label[0] != '\0')
		? scene_label
		: "<memory>";
	if (!loaded_any)
		LOG_WARN(LOG_TAG, "scene load: no supported entity data in %s", label);

	jce_state_clear_selection();
	if (scene_path && scene_path[0] != '\0') {
		/* Only update scene_dir (which clears the mesh/texture cache) when
		 * the scene path actually changed.  During undo/redo the path is
		 * identical, so we must avoid a cache wipe that forces every model
		 * to reload from disk. */
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
