/*
 * jce_editor_scene_parse.cpp  Scene JSON parsing and entity loading.
 *
 * Handles all JSON deserialization of scene files: component parsing,
 * entity tree loading, flat-array loading with parent references,
 * object-map loading, and contract version checking.
 */

#include "jce_editor_state_internal.h"

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
	bool has_parent;
	uint32_t new_id;
};

/* ── Component Type from Name ────────────────────────────────────── */

JceComponentType component_type_from_name(const char *name)
{
	if (!name) return JCE_COMP_TYPE_COUNT;

	static const struct { const char *name; JceComponentType type; } map[] = {
		{ "Transform",            JCE_COMP_TRANSFORM },
		{ "transform",            JCE_COMP_TRANSFORM },
		{ "MeshRenderer",         JCE_COMP_MESH_RENDERER },
		{ "Mesh Renderer",        JCE_COMP_MESH_RENDERER },
		{ "meshRenderer",         JCE_COMP_MESH_RENDERER },
		{ "mesh_renderer",        JCE_COMP_MESH_RENDERER },
		{ "SpriteRenderer",       JCE_COMP_SPRITE_RENDERER },
		{ "Sprite Renderer",      JCE_COMP_SPRITE_RENDERER },
		{ "spriteRenderer",       JCE_COMP_SPRITE_RENDERER },
		{ "sprite_renderer",      JCE_COMP_SPRITE_RENDERER },
		{ "Camera",               JCE_COMP_CAMERA },
		{ "camera",               JCE_COMP_CAMERA },
		{ "Light",                JCE_COMP_LIGHT },
		{ "light",                JCE_COMP_LIGHT },
		{ "Animator",             JCE_COMP_ANIMATOR },
		{ "animator",             JCE_COMP_ANIMATOR },
		{ "SkeletalAnimator",     JCE_COMP_SKELETAL_ANIMATOR },
		{ "Skeletal Animator",    JCE_COMP_SKELETAL_ANIMATOR },
		{ "Rigidbody",            JCE_COMP_RIGIDBODY },
		{ "rigidbody",            JCE_COMP_RIGIDBODY },
		{ "BoxCollider",          JCE_COMP_BOX_COLLIDER },
		{ "Box Collider",         JCE_COMP_BOX_COLLIDER },
		{ "SphereCollider",       JCE_COMP_SPHERE_COLLIDER },
		{ "Sphere Collider",      JCE_COMP_SPHERE_COLLIDER },
		{ "CharacterController",  JCE_COMP_CHARACTER_CONTROLLER },
		{ "Character Controller", JCE_COMP_CHARACTER_CONTROLLER },
		{ "AudioSource",          JCE_COMP_AUDIO_SOURCE },
		{ "Audio Source",         JCE_COMP_AUDIO_SOURCE },
		{ "Script",               JCE_COMP_SCRIPT },
		{ "script",               JCE_COMP_SCRIPT },
	};

	for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
		if (strcmp(name, map[i].name) == 0)
			return map[i].type;
	}
	return JCE_COMP_TYPE_COUNT;
}

/* ── JSON Number Helper ──────────────────────────────────────────── */

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

/* ── JSON String Helper ──────────────────────────────────────────── */

static const char *json_get_string(const cJSON *obj, const char *key)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL;
}

/* ── Per-component JSON parsers ─────────────────────────────────── */

static void parse_json_transform(const cJSON *props, JceComponentInfo *out)
{
	out->data.transform.pos[0]   = json_get_float_any2(props, "posX", "pos_x", 0.0f);
	out->data.transform.pos[1]   = json_get_float_any2(props, "posY", "pos_y", 0.0f);
	out->data.transform.pos[2]   = json_get_float_any2(props, "posZ", "pos_z", 0.0f);
	/* Rotation: check for quaternion (rotW present) and convert to Euler degrees. */
	{
		float rx = json_get_float_any2(props, "rotX", "rot_x", 0.0f);
		float ry = json_get_float_any2(props, "rotY", "rot_y", 0.0f);
		float rz = json_get_float_any2(props, "rotZ", "rot_z", 0.0f);
		const cJSON *rw_item = cJSON_GetObjectItemCaseSensitive(props, "rotW");
		if (!rw_item) rw_item = cJSON_GetObjectItemCaseSensitive(props, "rot_w");
		if (cJSON_IsNumber(rw_item)) {
			float qw = (float)rw_item->valuedouble;
			jce_quat q = {{ rx, ry, rz, qw }};
			q = jce_q_normalize(q);
			jce_vec3 e = jce_q_to_euler(q);
			rx = e.x * JCE_RAD2DEG;
			ry = e.y * JCE_RAD2DEG;
			rz = e.z * JCE_RAD2DEG;
		}
		out->data.transform.rot[0] = rx;
		out->data.transform.rot[1] = ry;
		out->data.transform.rot[2] = rz;
	}
	out->data.transform.scale[0] = json_get_float_any2(props, "scaleX", "scale_x", 1.0f);
	out->data.transform.scale[1] = json_get_float_any2(props, "scaleY", "scale_y", 1.0f);
	out->data.transform.scale[2] = json_get_float_any2(props, "scaleZ", "scale_z", 1.0f);
}

static void parse_json_camera(const cJSON *props, JceComponentInfo *out)
{
	out->data.camera.fov       = json_get_float(props, "fov", 60.0f);
	out->data.camera.near_clip = json_get_float_any2(props, "nearClip", "near_clip", 0.1f);
	out->data.camera.far_clip  = json_get_float_any2(props, "farClip", "far_clip", 1000.0f);
	bool ortho = false;
	static const char *const ortho_keys[] = { "orthographic", "ortho", "isOrtho" };
	json_get_bool_any(props, ortho_keys,
		(int)(sizeof(ortho_keys) / sizeof(ortho_keys[0])), &ortho);
	out->data.camera.ortho = ortho;
}

static void parse_json_light(const cJSON *props, JceComponentInfo *out)
{
	out->data.light.color[0]   = json_get_float_any2(props, "colorR", "color_r", 1.0f);
	out->data.light.color[1]   = json_get_float_any2(props, "colorG", "color_g", 1.0f);
	out->data.light.color[2]   = json_get_float_any2(props, "colorB", "color_b", 1.0f);
	out->data.light.color[3]   = json_get_float_any2(props, "colorA", "color_a", 1.0f);
	out->data.light.intensity  = json_get_float(props, "intensity", 1.0f);
	const cJSON *lt = cJSON_GetObjectItemCaseSensitive(props, "lightType");
	if (!lt) lt = cJSON_GetObjectItemCaseSensitive(props, "type");
	if (cJSON_IsNumber(lt))
		out->data.light.type = lt->valueint;
	else if (cJSON_IsString(lt) && lt->valuestring) {
		if (equals_ignore_case(lt->valuestring, "point")) out->data.light.type = 1;
		else if (equals_ignore_case(lt->valuestring, "spot")) out->data.light.type = 2;
	}
}

static void parse_json_mesh_renderer(const cJSON *props, JceComponentInfo *out)
{
	static const char *const mesh_keys[] = { "meshPath", "mesh_path", "mesh" };
	static const char *const mat_keys[] = { "materialPath", "material_path", "material" };
	const char *mesh = json_get_string_any(props, mesh_keys,
		(int)(sizeof(mesh_keys) / sizeof(mesh_keys[0])));
	const char *mat = json_get_string_any(props, mat_keys,
		(int)(sizeof(mat_keys) / sizeof(mat_keys[0])));
	if (mesh)
		snprintf(out->data.mesh_renderer.mesh_path,
		         sizeof(out->data.mesh_renderer.mesh_path), "%s", mesh);
	if (mat)
		snprintf(out->data.mesh_renderer.material_path,
		         sizeof(out->data.mesh_renderer.material_path), "%s", mat);
	out->data.mesh_renderer.mesh_shape = (int)json_get_float(props, "meshShape", 0.0f);
	/* PBR parameters. */
	out->data.mesh_renderer.base_color[0] = json_get_float(props, "baseColorR", 1.0f);
	out->data.mesh_renderer.base_color[1] = json_get_float(props, "baseColorG", 1.0f);
	out->data.mesh_renderer.base_color[2] = json_get_float(props, "baseColorB", 1.0f);
	out->data.mesh_renderer.base_color[3] = json_get_float(props, "baseColorA", 1.0f);
	out->data.mesh_renderer.metallic      = json_get_float(props, "metallic", 0.0f);
	out->data.mesh_renderer.roughness     = json_get_float(props, "roughness", 1.0f);
	out->data.mesh_renderer.emissive[0]   = json_get_float(props, "emissiveR", 0.0f);
	out->data.mesh_renderer.emissive[1]   = json_get_float(props, "emissiveG", 0.0f);
	out->data.mesh_renderer.emissive[2]   = json_get_float(props, "emissiveB", 0.0f);
	out->data.mesh_renderer.normal_scale  = json_get_float(props, "normalScale", 1.0f);
	out->data.mesh_renderer.ao_strength   = json_get_float(props, "aoStrength", 1.0f);
	out->data.mesh_renderer.alpha_mode    = (int)json_get_float(props, "alphaMode", 0.0f);
	out->data.mesh_renderer.alpha_cutoff  = json_get_float(props, "alphaCutoff", 0.5f);
	const cJSON *ds = cJSON_GetObjectItemCaseSensitive(props, "doubleSided");
	out->data.mesh_renderer.double_sided = cJSON_IsTrue(ds);
	/* Texture paths. */
	{ const char *v = json_get_string(props, "albedoTex");
	  if (v) snprintf(out->data.mesh_renderer.albedo_tex, 128, "%s", v); }
	{ const char *v = json_get_string(props, "mrTex");
	  if (v) snprintf(out->data.mesh_renderer.mr_tex, 128, "%s", v); }
	{ const char *v = json_get_string(props, "normalTex");
	  if (v) snprintf(out->data.mesh_renderer.normal_tex, 128, "%s", v); }
	{ const char *v = json_get_string(props, "aoTex");
	  if (v) snprintf(out->data.mesh_renderer.ao_tex, 128, "%s", v); }
	{ const char *v = json_get_string(props, "emissiveTex");
	  if (v) snprintf(out->data.mesh_renderer.emissive_tex, 128, "%s", v); }
}

static void parse_json_sprite_renderer(const cJSON *props, JceComponentInfo *out)
{
	{ const char *v = json_get_string(props, "spritePath");
	  if (v) snprintf(out->data.sprite_renderer.sprite_path, 128, "%s", v); }
	out->data.sprite_renderer.color[0] = json_get_float(props, "colorR", 1.0f);
	out->data.sprite_renderer.color[1] = json_get_float(props, "colorG", 1.0f);
	out->data.sprite_renderer.color[2] = json_get_float(props, "colorB", 1.0f);
	out->data.sprite_renderer.color[3] = json_get_float(props, "colorA", 1.0f);
	out->data.sprite_renderer.flip_x = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "flipX"));
	out->data.sprite_renderer.flip_y = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "flipY"));
	out->data.sprite_renderer.sorting_order = (int)json_get_float(props, "sortingOrder", 0.0f);
}

static void parse_json_animator(const cJSON *props, JceComponentInfo *out)
{
	{ const char *v = json_get_string(props, "clipName");
	  if (v) snprintf(out->data.animator.clip_name, 64, "%s", v); }
	out->data.animator.speed   = json_get_float(props, "speed", 1.0f);
	out->data.animator.loop    = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "loop"));
	out->data.animator.playing = false;
}

static void parse_json_skeletal_animator(const cJSON *props, JceComponentInfo *out)
{
	{ const char *v = json_get_string(props, "skeletonPath");
	  if (v) snprintf(out->data.skeletal_animator.skeleton_path, 128, "%s", v); }
	out->data.skeletal_animator.speed       = json_get_float(props, "speed", 1.0f);
	out->data.skeletal_animator.loop        = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "loop"));
	out->data.skeletal_animator.playing     = false;
	out->data.skeletal_animator.active_clip = (int)json_get_float(props, "activeClip", 0.0f);
	const cJSON *clips = cJSON_GetObjectItemCaseSensitive(props, "clipNames");
	out->data.skeletal_animator.clip_count = 0;
	if (cJSON_IsArray(clips)) {
		int n = cJSON_GetArraySize(clips);
		if (n > 8) n = 8;
		for (int ci = 0; ci < n; ci++) {
			const cJSON *ce = cJSON_GetArrayItem(clips, ci);
			if (cJSON_IsString(ce) && ce->valuestring)
				snprintf(out->data.skeletal_animator.clip_names[ci], 64, "%s", ce->valuestring);
		}
		out->data.skeletal_animator.clip_count = n;
	}
}

static void parse_json_rigidbody(const cJSON *props, JceComponentInfo *out)
{
	out->data.rigidbody.mass         = json_get_float(props, "mass", 1.0f);
	out->data.rigidbody.drag         = json_get_float(props, "drag", 0.0f);
	out->data.rigidbody.angular_drag = json_get_float(props, "angularDrag", 0.05f);
	out->data.rigidbody.use_gravity  = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "useGravity"));
	out->data.rigidbody.is_kinematic = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "isKinematic"));
	if (!cJSON_GetObjectItemCaseSensitive(props, "useGravity"))
		out->data.rigidbody.use_gravity = true;
}

static void parse_json_box_collider(const cJSON *props, JceComponentInfo *out)
{
	out->data.box_collider.center[0] = json_get_float(props, "centerX", 0.0f);
	out->data.box_collider.center[1] = json_get_float(props, "centerY", 0.0f);
	out->data.box_collider.center[2] = json_get_float(props, "centerZ", 0.0f);
	out->data.box_collider.size[0]   = json_get_float(props, "sizeX", 1.0f);
	out->data.box_collider.size[1]   = json_get_float(props, "sizeY", 1.0f);
	out->data.box_collider.size[2]   = json_get_float(props, "sizeZ", 1.0f);
	out->data.box_collider.is_trigger = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "isTrigger"));
}

static void parse_json_sphere_collider(const cJSON *props, JceComponentInfo *out)
{
	out->data.sphere_collider.center[0] = json_get_float(props, "centerX", 0.0f);
	out->data.sphere_collider.center[1] = json_get_float(props, "centerY", 0.0f);
	out->data.sphere_collider.center[2] = json_get_float(props, "centerZ", 0.0f);
	out->data.sphere_collider.radius     = json_get_float(props, "radius", 0.5f);
	out->data.sphere_collider.is_trigger = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "isTrigger"));
}

static void parse_json_character_controller(const cJSON *props, JceComponentInfo *out)
{
	out->data.character_controller.height      = json_get_float(props, "height", 2.0f);
	out->data.character_controller.radius      = json_get_float(props, "radius", 0.5f);
	out->data.character_controller.step_offset  = json_get_float(props, "stepOffset", 0.3f);
	out->data.character_controller.slope_limit  = json_get_float(props, "slopeLimit", 45.0f);
}

static void parse_json_audio_source(const cJSON *props, JceComponentInfo *out)
{
	{ const char *v = json_get_string(props, "clipPath");
	  if (v) snprintf(out->data.audio_source.clip_path, 128, "%s", v); }
	out->data.audio_source.volume        = json_get_float(props, "volume", 1.0f);
	out->data.audio_source.pitch         = json_get_float(props, "pitch", 1.0f);
	out->data.audio_source.spatial_blend  = json_get_float(props, "spatialBlend", 0.0f);
	out->data.audio_source.loop          = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "loop"));
	out->data.audio_source.play_on_awake = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(props, "playOnAwake"));
	if (!cJSON_GetObjectItemCaseSensitive(props, "playOnAwake"))
		out->data.audio_source.play_on_awake = true;
}

static void parse_json_script(const cJSON *props, JceComponentInfo *out)
{
	{ const char *v = json_get_string(props, "scriptPath");
	  if (v) snprintf(out->data.script.script_path, 128, "%s", v); }
}

/* ── Parse Component from JSON ───────────────────────────────────── */

static bool parse_component_json(const cJSON *comp_json, JceComponentInfo *out)
{
	if (!cJSON_IsObject(comp_json) || !out) return false;

	/* Determine component type. */
	static const char *const type_keys[] = { "type", "componentType", "class" };
	const char *type_str = json_get_string_any(comp_json, type_keys,
		(int)(sizeof(type_keys) / sizeof(type_keys[0])));
	if (!type_str) return false;

	JceComponentType type = component_type_from_name(type_str);
	if (type >= JCE_COMP_TYPE_COUNT) return false;

	memset(out, 0, sizeof(*out));
	out->type = type;
	out->expanded = true;

	const cJSON *props = cJSON_GetObjectItemCaseSensitive(comp_json, "properties");
	if (!cJSON_IsObject(props))
		props = comp_json;

	switch (type) {
	case JCE_COMP_TRANSFORM:            parse_json_transform(props, out);            break;
	case JCE_COMP_CAMERA:               parse_json_camera(props, out);               break;
	case JCE_COMP_LIGHT:                parse_json_light(props, out);                break;
	case JCE_COMP_MESH_RENDERER:        parse_json_mesh_renderer(props, out);        break;
	case JCE_COMP_SPRITE_RENDERER:      parse_json_sprite_renderer(props, out);      break;
	case JCE_COMP_ANIMATOR:             parse_json_animator(props, out);             break;
	case JCE_COMP_SKELETAL_ANIMATOR:    parse_json_skeletal_animator(props, out);    break;
	case JCE_COMP_RIGIDBODY:            parse_json_rigidbody(props, out);            break;
	case JCE_COMP_BOX_COLLIDER:         parse_json_box_collider(props, out);         break;
	case JCE_COMP_SPHERE_COLLIDER:      parse_json_sphere_collider(props, out);      break;
	case JCE_COMP_CHARACTER_CONTROLLER: parse_json_character_controller(props, out); break;
	case JCE_COMP_AUDIO_SOURCE:         parse_json_audio_source(props, out);         break;
	case JCE_COMP_SCRIPT:               parse_json_script(props, out);               break;
	default: break;
	}

	return true;
}

/* ── Parse Components Array for an Entity ────────────────────────── */

static void parse_entity_components(int entity_idx, const cJSON *obj)
{
	if (entity_idx < 0 || entity_idx >= s.entity_count) return;
	if (!cJSON_IsObject(obj)) return;

	static const char *const comp_keys[] = { "components", "component" };
	const cJSON *comps = json_get_any(obj, comp_keys,
		(int)(sizeof(comp_keys) / sizeof(comp_keys[0])));
	if (!cJSON_IsArray(comps)) return;

	int count = 0;
	for (cJSON *c = comps->child; c && count < JCE_MAX_COMPONENTS; c = c->next) {
		JceComponentInfo info;
		if (parse_component_json(c, &info))
			s.components[entity_idx][count++] = info;
	}
	s.entities[entity_idx].component_count = count;
}

/* ── Ensure Entity has a Transform Component ─────────────────────── */

static void ensure_transform_component(int entity_idx)
{
	if (entity_idx < 0 || entity_idx >= s.entity_count) return;

	JceEntityInfo *e = &s.entities[entity_idx];
	JceComponentInfo *comps = s.components[entity_idx];

	/* Check if already has Transform. */
	for (int i = 0; i < e->component_count; i++) {
		if (comps[i].type == JCE_COMP_TRANSFORM)
			return;
	}

	/* Add default Transform at the front. */
	if (e->component_count >= JCE_MAX_COMPONENTS) return;

	/* Shift existing components right. */
	for (int i = e->component_count; i > 0; i--)
		comps[i] = comps[i - 1];

	memset(&comps[0], 0, sizeof(comps[0]));
	comps[0].type = JCE_COMP_TRANSFORM;
	comps[0].expanded = true;
	comps[0].data.transform.scale[0] = 1.0f;
	comps[0].data.transform.scale[1] = 1.0f;
	comps[0].data.transform.scale[2] = 1.0f;
	e->component_count++;
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

static void apply_entity_fields(JceEntityInfo *e, const cJSON *obj)
{
	if (!e || !cJSON_IsObject(obj)) return;

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
		snprintf(e->name, sizeof(e->name), "%s", name);

	const char *tag = json_get_string_any(obj, tag_keys,
		(int)(sizeof(tag_keys) / sizeof(tag_keys[0])));
	if (tag)
		snprintf(e->tag, sizeof(e->tag), "%s", tag);

	bool enabled = true;
	if (json_get_bool_any(obj, enabled_keys,
		(int)(sizeof(enabled_keys) / sizeof(enabled_keys[0])),
		&enabled))
		e->enabled = enabled;

	bool prefab_instance = e->prefab_instance;
	if (json_get_bool_any(obj, prefab_instance_keys,
		(int)(sizeof(prefab_instance_keys) / sizeof(prefab_instance_keys[0])),
		&prefab_instance)) {
		e->prefab_instance = prefab_instance;
		if (!prefab_instance)
			e->prefab_path[0] = '\0';
	}

	const char *prefab_path = json_get_string_any(obj, prefab_path_keys,
		(int)(sizeof(prefab_path_keys) / sizeof(prefab_path_keys[0])));
	if (prefab_path && prefab_path[0] != '\0') {
		snprintf(e->prefab_path, sizeof(e->prefab_path), "%s", prefab_path);
		e->prefab_instance = true;
	}

	e->tag_color = parse_tag_color(obj);

	/* Parse components from JSON. */
	int idx = find_entity(e->id);
	if (idx >= 0) {
		parse_entity_components(idx, obj);
		ensure_transform_component(idx);
	}
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
	JceEntityInfo *e = jce_state_get_entity(id);
	apply_entity_fields(e, node);

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
		JceEntityInfo *e = jce_state_get_entity(id);
		apply_entity_fields(e, item);

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
		JceEntityInfo *e = jce_state_get_entity(id);
		apply_entity_fields(e, item);

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
		int before = s.entity_count;
		load_entities_from_array(container);
		loaded_any = (s.entity_count > before);
	} else if (cJSON_IsObject(container)) {
		static const char *const array_keys[] = {
			JCE_SCENE_ENTITIES_KEY, "objects", "nodes", "children"
		};
		const cJSON *entities_data = json_get_any(container, array_keys,
			(int)(sizeof(array_keys) / sizeof(array_keys[0])));
		if (cJSON_IsArray(entities_data)) {
			int before = s.entity_count;
			load_entities_from_array(entities_data);
			loaded_any = loaded_any || (s.entity_count > before);
		} else if (cJSON_IsObject(entities_data)) {
			int before = s.entity_count;
			load_entities_from_object_map(entities_data);
			loaded_any = loaded_any || (s.entity_count > before);
		}

		static const char *const root_node_keys[] = {
			"root", "sceneRoot", "hierarchyRoot"
		};
		const cJSON *root_node = json_get_any(container, root_node_keys,
			(int)(sizeof(root_node_keys) / sizeof(root_node_keys[0])));
		if (cJSON_IsObject(root_node)) {
			int before = s.entity_count;
			load_entity_tree_node(root_node, 0);
			loaded_any = loaded_any || (s.entity_count > before);
		}

		if (!loaded_any && looks_like_entity_object(container)) {
			int before = s.entity_count;
			load_entity_tree_node(container, 0);
			loaded_any = loaded_any || (s.entity_count > before);
		}
	}

	const char *label = (scene_label && scene_label[0] != '\0')
		? scene_label
		: "<memory>";
	if (!loaded_any)
		LOG_WARN(LOG_TAG, "scene load: no supported entity data in %s", label);

	jce_state_clear_selection();
	if (scene_path && scene_path[0] != '\0') {
		update_scene_dir_from_path(scene_path);
		set_current_scene_path_internal(scene_path);
	} else {
		set_current_scene_path_internal(NULL);
	}

	LOG_INFO(LOG_TAG, "scene loaded from %s (%d entities)", label, s.entity_count);
	return true;
}
