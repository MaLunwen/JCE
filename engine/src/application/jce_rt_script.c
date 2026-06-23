/*
 * jce_rt_script.c  Runtime script + behavior-tree module (split from
 * jce_runtime.c).
 *
 * The Lua JceScriptHost callback table (rt_script_*), the prefab/save/
 * hot-reload helpers behind it, and the bundled behavior-tree perception
 * actions (rt_bt_*).  Pure move from the monolithic runtime: cross-module
 * entry points are declared in jce_rt_internal.h, everything else stays
 * file-static here.  No behaviour change.
 */

#include "jce_rt_internal.h"

static void rt_script_log(void *user, const char *msg)
{
	(void)user;
	LOG_INFO(LOG_TAG, "[lua] %s", msg ? msg : "");
}

static bool rt_script_get_position(void *user, JceScriptEntity e, float out_xyz[3])
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return false;
	JceTransform *t = jce_scene_get_transform(rt->scene, (JceEntity)e);
	if (!t) return false;
	out_xyz[0] = t->position.x;
	out_xyz[1] = t->position.y;
	out_xyz[2] = t->position.z;
	return true;
}

static void rt_script_set_position(void *user, JceScriptEntity e,
                                   float x, float y, float z)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return;
	JceTransform *cur = jce_scene_get_transform(rt->scene, (JceEntity)e);
	if (!cur) return;
	JceTransform t = *cur;          /* copy to avoid src==dst aliasing in set */
	t.position.x = x;
	t.position.y = y;
	t.position.z = z;
	jce_scene_set_transform(rt->scene, (JceEntity)e, &t);
}

/* Read an asset's bytes host-fs-first (live editor source), then from the
 * mounted PAK + overlays (shipped builds where there is no source tree) —
 * the same host→PAK fallback terrain/audio/colliders use.  Returns a
 * jce_malloc'd buffer the caller frees with jce_free (matches the VM's
 * read_file contract and jce_fs_host_read_all's allocator), or NULL on miss. */
/* Map a scene-stored (typically project-relative) asset path to a host-
 * readable path via the optional host resolver (editor loose assets); returns
 * `buf` on success, else the original `path`.  The editor never chdir's and
 * mounts no global VFS in loose Play, so a raw relative path resolves against
 * the process CWD and misses — this routes every runtime host-fs asset load
 * through the SAME resolution the renderer uses, so Play physics/logic loads
 * exactly what the editor visuals show.  PAK lookups keep the original path
 * (pak keys are the canonical project-relative form). */
const char *rt_resolve_host_path(JceRuntime *rt, const char *path,
                                        char *buf, size_t cap)
{
	if (rt && rt->resolve_path_fn && path && path[0] && buf && cap &&
	    rt->resolve_path_fn(rt->user_data, path, buf, (int)cap))
		return buf;
	return path;
}

void *rt_read_asset_with_fallback(JceRuntime *rt, const char *path,
                                         uint64_t *out_size)
{
	if (out_size) *out_size = 0;
	if (!path || !path[0]) return NULL;
	/* Editor / dev: live file on the host filesystem (resolved so loose
	 * project-relative paths work regardless of the process CWD). */
	char        rbuf[1024];
	const char *host_path = rt_resolve_host_path(rt, path, rbuf, sizeof rbuf);
	void *buf = jce_fs_host_read_all(host_path, out_size);
	if (buf) return buf;
	/* Shipped: the cooked, packed copy in the mounted PAK (+ bundle overlays). */
	if (rt && rt->pak) {
		const JcePakAsset *a = jce_pak_find(rt->pak, path);
		if (a && a->original_size > 0) {
			void *pbuf = jce_malloc((size_t)a->original_size);
			if (pbuf) {
				size_t n = jce_pak_decompress(a, pbuf, (size_t)a->original_size);
				if (n > 0) {
					if (out_size) *out_size = (uint64_t)n;
					return pbuf;
				}
				jce_free(pbuf);
			}
		}
	}
	return NULL;
}

static void *rt_script_read_file(void *user, const char *path, uint64_t *out_size)
{
	/* Host filesystem in the editor; PAK-resident in shipped builds. */
	return rt_read_asset_with_fallback((JceRuntime *)user, path, out_size);
}

/* Forward decls: time-control public API used by the script bridge below. */
JCE_API void  JCE_CALL jce_runtime_set_time_scale(JceRuntime *rt, float scale);
JCE_API void  JCE_CALL jce_runtime_set_paused(JceRuntime *rt, bool paused);

static void rt_script_set_time_scale(void *user, float scale)
{
	jce_runtime_set_time_scale((JceRuntime *)user, scale);
}

static void rt_script_set_paused(void *user, bool paused)
{
	jce_runtime_set_paused((JceRuntime *)user, paused);
}

/* Forward decl: camera-shake seam used by the script bridge below. */
JCE_API void  JCE_CALL jce_runtime_shake_camera(JceRuntime *rt, float amount);

static void rt_script_shake_camera(void *user, float amount)
{
	jce_runtime_shake_camera((JceRuntime *)user, amount);
}

/* ── Adaptive music director script bridge (FEATURE 5.3) ──────────────
 * Act on the runtime's single live JceMusicDirector (built from the scene's
 * MusicTrack component).  All no-op / default when no director exists. */
static void rt_script_music_set_intensity(void *user, float intensity)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (rt && rt->music) jce_music_set_intensity(rt->music, intensity);
}

static float rt_script_music_get_intensity(void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	return (rt && rt->music) ? jce_music_get_intensity(rt->music) : 0.0f;
}

static float rt_script_music_request_transition(void *user, int to_segment)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->music) return -1.0f;
	/* Quantize to the next bar boundary — the musical default for a segment
	 * switch (beat-level switches are jarring for full re-sequences). */
	return jce_music_request_transition(rt->music, to_segment,
	                                    JCE_MUSIC_QUANT_BAR);
}

/* ── GAS script bridge (consumption last-mile) ──────────────────────── */

static bool rt_script_gas_activate(void *user, JceScriptEntity e,
                                   uint32_t ability_id)
{
	JceGameplayAbilitySystem *gas = rt_gas_for_entity((JceRuntime *)user,
	                                                  (JceEntity)e);
	if (!gas) return false;
	return jce_gas_activate_ability(gas, ability_id);
}

static bool rt_script_gas_get(void *user, JceScriptEntity e,
                              const char *attr_name, float *out_value)
{
	JceGameplayAbilitySystem *gas = rt_gas_for_entity((JceRuntime *)user,
	                                                  (JceEntity)e);
	if (!gas || !attr_name || !out_value) return false;
	int32_t idx = jce_attribute_set_find(&gas->attributes, attr_name);
	if (idx < 0) return false;
	*out_value = jce_gas_attribute_current(gas, idx);
	return true;
}

static bool rt_script_gas_apply(void *user, JceScriptEntity e,
                                const char *attr_name, int op, float magnitude,
                                float duration_seconds)
{
	JceGameplayAbilitySystem *gas = rt_gas_for_entity((JceRuntime *)user,
	                                                  (JceEntity)e);
	if (!gas || !attr_name) return false;
	int32_t idx = jce_attribute_set_find(&gas->attributes, attr_name);
	if (idx < 0) return false;

	JceGameplayEffect eff;
	memset(&eff, 0, sizeof eff);
	eff.attr_idx = idx;
	eff.op = (op == 1) ? JCE_GAS_OP_MULT
	       : (op == 2) ? JCE_GAS_OP_OVERRIDE
	                   : JCE_GAS_OP_ADD;
	eff.magnitude = magnitude;
	if (duration_seconds > 0.0f) {
		/* A continuous TIMED modifier (folds into CURRENT while alive). */
		eff.duration_mode    = JCE_GAS_DURATION_TIMED;
		eff.duration_seconds = duration_seconds;
		eff.period_seconds   = 0.0f;
		eff.stack_policy     = JCE_GAS_STACK_REPLACE;
	} else {
		/* INSTANT: permanently change base (instant damage / heal). */
		eff.duration_mode = JCE_GAS_DURATION_INSTANT;
	}
	/* INSTANT returns 0 by contract but still applies; treat both as success. */
	jce_gas_apply_effect(gas, &eff);
	return true;
}

static bool rt_script_get_rotation(void *user, JceScriptEntity e, float out[3])
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return false;
	JceTransform *t = jce_scene_get_transform(rt->scene, (JceEntity)e);
	if (!t) return false;
	jce_vec3 eul = jce_q_to_euler(t->rotation);   /* radians (pitch,yaw,roll) */
	out[0] = eul.x * JCE_RAD2DEG;
	out[1] = eul.y * JCE_RAD2DEG;
	out[2] = eul.z * JCE_RAD2DEG;
	return true;
}

static void rt_script_set_rotation(void *user, JceScriptEntity e,
                                   float x, float y, float z)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return;
	JceTransform *cur = jce_scene_get_transform(rt->scene, (JceEntity)e);
	if (!cur) return;
	JceTransform t = *cur;
	t.rotation = jce_euler_to_q(x, y, z);          /* degrees */
	jce_scene_set_transform(rt->scene, (JceEntity)e, &t);
}

static bool rt_script_get_scale(void *user, JceScriptEntity e, float out[3])
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return false;
	JceTransform *t = jce_scene_get_transform(rt->scene, (JceEntity)e);
	if (!t) return false;
	out[0] = t->scale.x; out[1] = t->scale.y; out[2] = t->scale.z;
	return true;
}

static void rt_script_set_scale(void *user, JceScriptEntity e,
                                float x, float y, float z)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return;
	JceTransform *cur = jce_scene_get_transform(rt->scene, (JceEntity)e);
	if (!cur) return;
	JceTransform t = *cur;
	t.scale.x = x; t.scale.y = y; t.scale.z = z;
	jce_scene_set_transform(rt->scene, (JceEntity)e, &t);
}

static JceScriptEntity rt_script_find_with_tag(void *user, const char *tag)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !tag) return 0;
	return (JceScriptEntity)jce_scene_find_with_tag(rt->scene, tag);
}

static void rt_script_destroy_entity(void *user, JceScriptEntity e)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || e == 0) return;
	jce_scene_destroy_entity(rt->scene, (JceEntity)e);
}

static void rt_script_move_axis(void *user, float out[2])
{
	JceRuntime *rt = (JceRuntime *)user;
	out[0] = rt ? rt->input.walk_x : 0.0f;
	out[1] = rt ? rt->input.walk_z : 0.0f;
}

static bool rt_script_input_button(void *user, int button)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt) return false;
	if (button == 0) return rt->input.jump_pressed || rt->input.jump_held;
	if (button == 1) return rt->input.sprint;
	if (button == 2) return rt->input.attack_pressed;
	return false;
}

/* ── Physics / audio / UI script bridge (scripting-depth slice) ─────────────
 * Forward decl: the entity→body lookup the joints pass also uses (defined far
 * below, after the spawn pass).  The script physics bindings resolve a body
 * the same way. */

/* jce.raycast: cast a ray through the live physics world; map the hit body
 * back to its tagged entity (0 if untagged).  Returns false (binding sees a
 * miss) when there is no physics world or the ray hits nothing. */
static bool rt_script_raycast(void *user, const float origin[3],
                              const float dir[3], float max_dist,
                              JceScriptRaycastHit *out)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->physics || !origin || !dir || !out) return false;
	jce_vec3 o = jce_v3(origin[0], origin[1], origin[2]);
	jce_vec3 d = jce_v3(dir[0], dir[1], dir[2]);
	JceRaycastResult r = jce_physics_raycast(rt->physics, o, d, max_dist);
	if (!r.hit) return false;
	out->entity   = (JceScriptEntity)jce_physics_body_get_entity(rt->physics, r.body);
	out->point[0]  = r.point.x;  out->point[1]  = r.point.y;  out->point[2]  = r.point.z;
	out->normal[0] = r.normal.x; out->normal[1] = r.normal.y; out->normal[2] = r.normal.z;
	out->distance  = r.distance;
	return true;
}

/* Foot-IK ground query: physics raycast against rt->physics, exposed for the
 * scene renderer's ground-query hook (mirrors rt_script_raycast). */
JCE_API bool JCE_CALL jce_runtime_ground_raycast(JceRuntime *rt,
                                                 const float origin[3],
                                                 const float dir[3],
                                                 float max_dist,
                                                 float *out_hit_y,
                                                 float out_normal[3])
{
	if (!rt || !rt->physics || !origin || !dir) return false;
	jce_vec3 o = jce_v3(origin[0], origin[1], origin[2]);
	jce_vec3 d = jce_v3(dir[0], dir[1], dir[2]);
	JceRaycastResult r = jce_physics_raycast(rt->physics, o, d, max_dist);
	if (!r.hit) return false;
	if (out_hit_y)  *out_hit_y = r.point.y;
	if (out_normal) { out_normal[0] = r.normal.x; out_normal[1] = r.normal.y;
	                  out_normal[2] = r.normal.z; }
	return true;
}

/* jce.apply_impulse: instantaneous impulse on the entity's dynamic body. */
static void rt_script_apply_impulse(void *user, JceScriptEntity e,
                                    float x, float y, float z)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->physics) return;
	JceBodyHandle b = rt_body_for_entity(rt, (JceEntity)e);
	if (!jce_body_valid(b)) return;
	jce_physics_body_apply_impulse(rt->physics, b, jce_v3(x, y, z));
}

/* jce.set_velocity: set the entity body's linear velocity (m/s). */
static void rt_script_set_velocity(void *user, JceScriptEntity e,
                                   float x, float y, float z)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->physics) return;
	JceBodyHandle b = rt_body_for_entity(rt, (JceEntity)e);
	if (!jce_body_valid(b)) return;
	jce_physics_body_set_velocity(rt->physics, b, jce_v3(x, y, z));
}

/* jce.anim_set_*: queue an animator SM param command onto the scene relay; the
 * scene renderer drains it into the entity's SM binding next frame. */
static void rt_anim_push(JceRuntime *rt, JceScriptEntity e, int type,
                         const char *name, float v)
{
	if (!rt || !rt->scene || !name) return;
	JceAnimParamCmd cmd;
	memset(&cmd, 0, sizeof cmd);
	cmd.type  = type;
	snprintf(cmd.name, sizeof cmd.name, "%s", name);
	cmd.value = v;
	jce_scene_anim_push_param(rt->scene, (JceEntity)e, &cmd);
}
static void rt_script_anim_set_float(void *user, JceScriptEntity e, const char *name, float v)
{ rt_anim_push((JceRuntime *)user, e, JCE_ANIM_PARAM_FLOAT, name, v); }
static void rt_script_anim_set_int(void *user, JceScriptEntity e, const char *name, int v)
{ rt_anim_push((JceRuntime *)user, e, JCE_ANIM_PARAM_INT, name, (float)v); }
static void rt_script_anim_set_bool(void *user, JceScriptEntity e, const char *name, bool v)
{ rt_anim_push((JceRuntime *)user, e, JCE_ANIM_PARAM_BOOL, name, v ? 1.0f : 0.0f); }
static void rt_script_anim_set_trigger(void *user, JceScriptEntity e, const char *name)
{ rt_anim_push((JceRuntime *)user, e, JCE_ANIM_PARAM_TRIGGER, name, 0.0f); }

/* Data-driven action queries (jce.is_action_down / is_action_pressed / get_axis):
 * resolve the named action against the borrowed live action map; safe default
 * (false / 0) when no map is bound this frame or the name is unknown. */
static bool rt_script_action_down(void *user, const char *name)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->actions || !name) return false;
	int id = jce_action_find(rt->actions, name);
	return id >= 0 && jce_action_down(rt->actions, id);
}
static bool rt_script_action_pressed(void *user, const char *name)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->actions || !name) return false;
	int id = jce_action_find(rt->actions, name);
	return id >= 0 && jce_action_pressed(rt->actions, id);
}
static float rt_script_action_axis(void *user, const char *name)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->actions || !name) return 0.0f;
	int id = jce_action_find(rt->actions, name);
	return id >= 0 ? jce_action_value(rt->actions, id) : 0.0f;
}

/* jce.get_velocity: read the entity body's linear velocity into out[3]. */
static bool rt_script_get_velocity(void *user, JceScriptEntity e, float out[3])
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->physics || !out) return false;
	JceBodyHandle b = rt_body_for_entity(rt, (JceEntity)e);
	if (!jce_body_valid(b)) return false;
	jce_vec3 v = jce_physics_body_get_velocity(rt->physics, b);
	out[0] = v.x; out[1] = v.y; out[2] = v.z;
	return true;
}

/* jce.vehicle_set_input / jce.vehicle_get_speed: drive / query an authored
 * Vehicle (SCRIPT input mode) bound to the entity.  Mirror the public
 * jce_runtime_vehicle_* API but resolve the live VehicleEntry directly so they
 * have no forward-declaration dependency. */
static void rt_script_vehicle_set_input(void *user, JceScriptEntity e,
                                        float throttle, float brake, float steer)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->physics) return;
	VehicleEntry *ve = rt_vehicle_for_entity(rt, (JceEntity)e);
	if (!ve) return;
	jce_physics_vehicle_set_input(rt->physics, ve->veh, throttle, brake, steer);
}
static float rt_script_vehicle_get_speed(void *user, JceScriptEntity e)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->physics) return 0.0f;
	VehicleEntry *ve = rt_vehicle_for_entity(rt, (JceEntity)e);
	if (!ve) return 0.0f;
	return jce_physics_vehicle_get_speed(rt->physics, ve->veh);
}

/* jce.get_move: expose the raw movement intent the host fed this frame so a
 * script (e.g. car_controller in SCRIPT vehicle mode) can read WASD without an
 * authored action map.  out = {walk_x steer, walk_z throttle, brake 0/1}. */
static void rt_script_get_move(void *user, float out[3])
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!out) return;
	if (!rt) { out[0] = out[1] = out[2] = 0.0f; return; }
	out[0] = rt->input.walk_x;
	out[1] = rt->input.walk_z;
	out[2] = (rt->input.jump_pressed || rt->input.jump_held) ? 1.0f : 0.0f;
}

/* jce.play_sound: load (host-fs → PAK) and play a one-shot sound by path.
 * `pos` non-NULL makes it positional (3D); NULL is a 2D sound.  Loads the
 * sound on each call (the audio sound table dedups by path internally), and
 * is a safe no-op on a missing file / when audio is unavailable. */
static void rt_script_play_sound(void *user, const char *path,
                                 const float pos[3], float volume)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->audio || !path || !path[0]) return;
	JceSound snd = jce_audio_load(rt->audio, rt->pak, path);
	if (snd == JCE_SOUND_INVALID) {
		LOG_WARN(LOG_TAG, "jce.play_sound: cannot load '%s'", path);
		return;
	}
	float vol = (volume > 0.0f) ? volume : 1.0f;
	JceVoice v = jce_audio_play(rt->audio, snd, false, vol, 1.0f);
	if (pos) {
		jce_audio_voice_set_3d(rt->audio, v, true);
		jce_audio_voice_set_position(rt->audio, v, pos[0], pos[1], pos[2]);
		jce_audio_voice_set_attenuation(rt->audio, v,
		                                JCE_AUDIO_ATTEN_INVERSE,
		                                1.0f, 25.0f, 1.0f);
	} else {
		jce_audio_voice_set_3d(rt->audio, v, false);
	}
}

/* jce.ui_get_slider / ui_set_slider: the live UISlider's value. */
static bool rt_script_ui_get_slider(void *user, JceScriptEntity e, float *out)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !out) return false;
	JceUISliderComponent *sl = jce_scene_get_ui_slider(rt->scene, (JceEntity)e);
	if (!sl) return false;
	*out = sl->value;
	return true;
}

static void rt_script_ui_set_slider(void *user, JceScriptEntity e, float v)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return;
	JceUISliderComponent *sl = jce_scene_get_ui_slider(rt->scene, (JceEntity)e);
	if (!sl) return;
	sl->value = v;   /* MUTABLE accessor: write the new value back in place */
}

/* jce.ui_get_toggle / ui_set_toggle: the live UIToggle's on/off state. */
static bool rt_script_ui_get_toggle(void *user, JceScriptEntity e, bool *out)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !out) return false;
	JceUIToggleComponent *tg = jce_scene_get_ui_toggle(rt->scene, (JceEntity)e);
	if (!tg) return false;
	*out = tg->is_on;
	return true;
}

static void rt_script_ui_set_toggle(void *user, JceScriptEntity e, bool v)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return;
	JceUIToggleComponent *tg = jce_scene_get_ui_toggle(rt->scene, (JceEntity)e);
	if (!tg) return;
	tg->is_on = v;   /* MUTABLE accessor: write the new state back in place */
}

/* jce.ui_set_text: replace the live UIText's displayed string (bounds-safe). */
static void rt_script_ui_set_text(void *user, JceScriptEntity e, const char *txt)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !txt) return;
	JceUITextComponent *ut = jce_scene_get_ui_text(rt->scene, (JceEntity)e);
	if (!ut) return;
	size_t n = strlen(txt);
	if (n >= sizeof ut->text) n = sizeof ut->text - 1;
	memcpy(ut->text, txt, n);
	ut->text[n] = '\0';
}

/* Physics contact → script bridge: on each BEGIN contact, dispatch
 * on_collision(self, other_entity) to any scripted entity involved.  Registered
 * as a physics contact listener in jce_runtime_create (when physics + the
 * script VM both exist) and removed in jce_runtime_destroy.  entity_a/b come
 * from jce_physics_body_set_entity; a 0 tag (e.g. the character-controller
 * capsule, which is not tagged) makes that side dispatch nothing. */
void rt_script_collision_cb(const JceContactEvent *ev, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (!rt || !rt->script_vm || !ev) return;
	if (ev->type != JCE_CONTACT_BEGIN) return;   /* fire once per contact pair */
	for (int i = 0; i < rt->script_count; ++i) {
		struct ScriptEntry *se = &rt->scripts[i];
		if (!se->active) continue;
		if ((uint64_t)se->entity == ev->entity_a && ev->entity_b != 0)
			jce_script_call_collision(rt->script_vm, se->inst,
			                          (JceScriptEntity)ev->entity_b);
		else if ((uint64_t)se->entity == ev->entity_b && ev->entity_a != 0)
			jce_script_call_collision(rt->script_vm, se->inst,
			                          (JceScriptEntity)ev->entity_a);
	}
}

/* jce.send_message(target, msg, number, string): script-to-script messaging.
 * Resolve `target` to its live script instance (the VM keeps no entity->instance
 * map; the runtime does, in rt->scripts[]) and dispatch method `msg` on it with
 * the (number, string) payload.  No-op if the target has no script (not found),
 * no live instance, or no handler named `msg` (the latter is tolerated inside
 * jce_script_call_message).  Mirrors rt_script_collision_cb's linear scan. */
static void rt_script_send_message(void *user, JceScriptEntity target,
                                   const char *msg, double number_arg,
                                   const char *str_arg)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->script_vm || !msg) return;
	for (int i = 0; i < rt->script_count; ++i) {
		struct ScriptEntry *se = &rt->scripts[i];
		if (!se->active) continue;
		if ((JceScriptEntity)se->entity == target) {
			jce_script_call_message(rt->script_vm, se->inst, msg,
			                        number_arg, str_arg);
			return;   /* one instance per entity; first match wins */
		}
	}
}

/* jce.broadcast(msg, number, string): global event bus — dispatch method `msg`
 * to EVERY live script instance (sender included).  Instances defining no such
 * handler are skipped (tolerated inside jce_script_call_message).  Mirrors
 * rt_script_send_message's scan, minus the entity filter. */
static void rt_script_broadcast(void *user, const char *msg,
                                double number_arg, const char *str_arg)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->script_vm || !msg) return;
	for (int i = 0; i < rt->script_count; ++i) {
		struct ScriptEntry *se = &rt->scripts[i];
		if (!se->active) continue;
		jce_script_call_message(rt->script_vm, se->inst, msg,
		                        number_arg, str_arg);
	}
}

/* jce.has_component / is_component_enabled / set_component_enabled: lightweight
 * runtime reflection over the dense component registry, resolving the component
 * BY NAME.  Unknown name → safe default (query false / set no-op). */
static bool rt_script_has_component(void *user, JceScriptEntity e,
                                    const char *comp_name)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !comp_name) return false;
	int cid = jce_component_find(comp_name);
	if (cid < 0) return false;
	return jce_scene_has_comp(rt->scene, (JceEntity)e, cid);
}

static bool rt_script_is_component_enabled(void *user, JceScriptEntity e,
                                           const char *comp_name)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !comp_name) return false;
	int cid = jce_component_find(comp_name);
	if (cid < 0) return false;
	return jce_scene_has_comp(rt->scene, (JceEntity)e, cid) &&
	       jce_scene_comp_enabled(rt->scene, (JceEntity)e, cid);
}

static void rt_script_set_component_enabled(void *user, JceScriptEntity e,
                                            const char *comp_name, bool on)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !comp_name) return;
	int cid = jce_component_find(comp_name);
	if (cid < 0) return;
	if (jce_scene_has_comp(rt->scene, (JceEntity)e, cid))
		jce_scene_set_comp_enabled(rt->scene, (JceEntity)e, cid, on);
}

/* jce.spawn(prefab_path, x, y, z): instantiate a prefab into the live scene at
 * (x,y,z) and return its root entity.  The prefab JSON is read host-fs-first
 * then from the PAK (shipped); entities are created immediately so the id can
 * be returned synchronously, but their physics + gameplay wiring is deferred to
 * rt_flush_pending_spawns (after the script update loop) so appending to
 * scripts[] can't realloc mid-iteration.  Returns 0 on failure or when called
 * during the initial create() scene walk (adding entities there would corrupt
 * the in-flight ECS query iterator — call jce.spawn from on_update instead). */
/* Instantiate a prefab (project-relative path) at a world position; queues the
 * new entities for deferred physics/gameplay wiring.  Returns the root entity
 * (0 on failure).  Shared by jce.spawn and the SpawnManager. */
JceEntity rt_spawn_prefab_at(JceRuntime *rt, const char *prefab_path,
                                    float x, float y, float z)
{
	if (!rt || !rt->scene || !prefab_path || !prefab_path[0]) return 0;

	uint64_t size = 0;
	void *json = rt_read_asset_with_fallback(rt, prefab_path, &size);
	if (!json || size == 0) {
		if (json) jce_free(json);
		LOG_WARN(LOG_TAG, "spawn: cannot read prefab '%s'", prefab_path);
		return 0;
	}

	JceEntity *ents = NULL;
	uint32_t   n    = 0;
	bool ok = jce_scene_serial_load_additive(rt->scene, (const char *)json,
	                                          (size_t)size, &ents, &n);
	jce_free(json);
	if (!ok || n == 0 || !ents) {
		if (ents) jce_scene_serial_free_entities(ents);
		LOG_WARN(LOG_TAG, "spawn: failed to instantiate '%s'", prefab_path);
		return 0;
	}

	JceEntity root = ents[0];

	/* Place the root subtree at the requested spawn position. */
	JceTransform *cur = jce_scene_get_transform(rt->scene, root);
	if (cur) {
		JceTransform t = *cur;
		t.position.x = x;
		t.position.y = y;
		t.position.z = z;
		jce_scene_set_transform(rt->scene, root, &t);
	}

	/* Queue every new entity for deferred physics + gameplay wiring. */
	for (uint32_t i = 0; i < n; ++i) {
		if (rt->pending_spawn_count >= rt->pending_spawn_cap &&
		    !rt_grow_pending_spawns(rt))
			break;
		rt->pending_spawns[rt->pending_spawn_count++] = ents[i];
	}
	jce_scene_serial_free_entities(ents);
	return root;
}

static JceScriptEntity rt_script_spawn(void *user, const char *prefab_path,
                                       float x, float y, float z)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !prefab_path || !prefab_path[0]) return 0;
	if (rt->in_scene_walk) {
		LOG_WARN(LOG_TAG, "jce.spawn('%s') ignored: not allowed from on_start "
		         "during initial scene load — call it from on_update", prefab_path);
		return 0;
	}
	return (JceScriptEntity)rt_spawn_prefab_at(rt, prefab_path, x, y, z);
}

/* jce.net_is_server(): true when the live session is the authoritative server
 * (host or dedicated).  Mirrors the gate already used by the runtime's own
 * server-side update paths (see jce_session_is_server() callers). */
static bool rt_script_net_is_server(void *user)
{
	(void)user;
	return jce_session_is_server();
}

/* jce.net_is_client(): true when the live session is a connected client (not the
 * server).  Mirrors jce_session_is_client(), the runtime's own client gate. */
static bool rt_script_net_is_client(void *user)
{
	(void)user;
	return jce_session_is_client();
}

/* jce.net_spawn(prefab_path, x, y, z): server-authoritative networked spawn.
 * Server-only by the replication contract (jce_net_object_spawn returns
 * JCE_NET_OBJECT_INVALID off-server), so this returns 0 on a client / failure.
 * On success the new NetworkObject's backing flecs entity is resolved via
 * jce_net_object_to_entity and placed at the requested world position, then the
 * returned entity id flows back to the script.  The replication subsystem
 * broadcasts the spawn to clients on its next tick. */
static JceScriptEntity rt_script_net_spawn(void *user, const char *prefab_path,
                                           float x, float y, float z)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !prefab_path || !prefab_path[0]) return 0;
	if (!jce_session_is_server()) return 0;   /* server-authoritative */

	JceNetObjectDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.prefab_path = prefab_path;
	desc.owner       = JCE_CLIENT_SERVER;
	desc.flags       = 0;

	JceNetObjectId id = jce_net_object_spawn(&desc);
	if (id == JCE_NET_OBJECT_INVALID) {
		LOG_WARN(LOG_TAG, "jce.net_spawn: failed to spawn NetworkObject for "
		         "prefab '%s'", prefab_path);
		return 0;
	}

	uint64_t entity = jce_net_object_to_entity(id);
	if (entity == 0) return 0;   /* spawned but not mapped locally */

	/* Place the spawned NetworkObject at the requested world position. */
	JceTransform *cur = jce_scene_get_transform(rt->scene, (JceEntity)entity);
	if (cur) {
		JceTransform t = *cur;
		t.position.x = x;
		t.position.y = y;
		t.position.z = z;
		jce_scene_set_transform(rt->scene, (JceEntity)entity, &t);
	}
	return (JceScriptEntity)entity;
}

/* jce.particle_burst(entity, count): fire a one-shot burst from the entity's
 * live particle emitter (no-op when the entity has no JceParticleEmitterComponent
 * / its emitter is not built yet — see jce_scene_particle_burst). */
static void rt_script_particle_burst(void *user, JceScriptEntity e, int count)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return;
	jce_scene_particle_burst(rt->scene, (JceEntity)e, count);
}

/* jce.particle_set_emitting(entity, on): start/stop the entity emitter's
 * continuous emission (no-op when the entity has no emitter). */
static void rt_script_particle_set_emitting(void *user, JceScriptEntity e, bool on)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return;
	jce_scene_particle_set_emitting(rt->scene, (JceEntity)e, on);
}

/* File-watcher callback: a script's source changed on disk → hot-reload it. */
void rt_on_script_changed(const char *path, void *user)
{
	jce_runtime_reload_script((JceRuntime *)user, path);
}

/* Write a session snapshot to "<saves_dir>/<save_id>.jsnp" through the
 * runtime registry, creating the saves directory on demand (P2-save-snapshot).
 * No-op (returns false) when no saves directory was configured. */
bool rt_perform_save(JceRuntime *rt, const char *save_id)
{
	if (!rt || !rt->save_registry) return false;
	if (rt->saves_dir[0] == '\0') {
		LOG_WARN(LOG_TAG, "save point fired but no saves_dir configured");
		return false;
	}
	if (!save_id || !save_id[0]) save_id = "checkpoint";

	if (!jce_fs_host_exists_dir(rt->saves_dir) &&
	    !jce_fs_host_create_directory(rt->saves_dir)) {
		LOG_ERROR(LOG_TAG, "save: cannot create directory '%s'", rt->saves_dir);
		return false;
	}

	char path[640];
	int n = snprintf(path, sizeof path, "%s/%s.jsnp", rt->saves_dir, save_id);
	if (n <= 0 || (size_t)n >= sizeof path) return false;

	bool ok = jce_snapshot_save_to_file(rt->save_registry, path);
	if (ok) LOG_INFO(LOG_TAG, "save point: wrote snapshot '%s'", path);
	else    LOG_ERROR(LOG_TAG, "save point: write failed '%s'", path);
	return ok;
}


/* ── Behavior-tree perception + actions (P2-perception-bt-binding) ─────
 *
 * Line-of-sight adapter: the perception module asks "is the segment
 * from→to BLOCKED?".  We raycast the physics world (closest hit, no
 * triggers) and report blocked when something is hit short of the target.
 * A small epsilon keeps the target's own collider (right at `to`) from
 * counting as an occluder. */
bool rt_bt_los_blocked(jce_vec3 from, jce_vec3 to, void *userdata)
{
	JceRuntime *rt = (JceRuntime *)userdata;
	if (!rt || !rt->physics) return false;   /* no physics → assume clear */
	jce_vec3 seg = jce_v3_sub(to, from);
	float dist = jce_v3_len(seg);
	if (dist <= 1e-4f) return false;
	jce_vec3 dir = jce_v3_scale(seg, 1.0f / dist);
	JceQueryFilter filter = jce_query_filter_default();   /* skip triggers */
	JceRaycastResult r = jce_physics_raycast_filtered(rt->physics, from, dir,
	                                                  dist, filter);
	if (!r.hit) return false;
	/* Hit something before reaching the target (minus a small skin) → blocked. */
	return r.distance < (dist - 0.1f);
}

/* The bundled BT actions operate on rt->bt_active_bb (the blackboard of the
 * agent currently being ticked).  These are deliberately generic primitives
 * so an authored tree can react to perception without any game C code:
 *
 *   IsTargetVisible — SUCCESS when perception saw a target this tick.
 *   HasTarget       — SUCCESS when a target entity is known (seen now OR a
 *                     last-known position was recorded).
 *   HasHeardSound   — SUCCESS when a sound was heard this tick.
 *   IsTargetInRange — SUCCESS when target.distance <= "attack.range"
 *                     (default 2m) — a melee/attack gate.
 *
 * Game-specific actions (MoveTo, Attack, …) stay in game code: it registers
 * them on jce_runtime_bt_context() before Play, and they read the same
 * blackboard via jce_runtime_bt_blackboard(). */
static JceBtStatus rt_bt_action_is_visible(const char *name, void *ud)
{
	(void)name;
	JceRuntime *rt = (JceRuntime *)ud;
	if (!rt || !rt->bt_active_bb) return JCE_BT_FAILURE;
	return jce_blackboard_get_bool(rt->bt_active_bb, "target.visible", false)
	       ? JCE_BT_SUCCESS : JCE_BT_FAILURE;
}

static JceBtStatus rt_bt_action_has_target(const char *name, void *ud)
{
	(void)name;
	JceRuntime *rt = (JceRuntime *)ud;
	if (!rt || !rt->bt_active_bb) return JCE_BT_FAILURE;
	if (jce_blackboard_get_entity(rt->bt_active_bb, "target.entity", 0) != 0)
		return JCE_BT_SUCCESS;
	return jce_blackboard_has(rt->bt_active_bb, "target.last_known_position")
	       ? JCE_BT_SUCCESS : JCE_BT_FAILURE;
}

static JceBtStatus rt_bt_action_has_heard(const char *name, void *ud)
{
	(void)name;
	JceRuntime *rt = (JceRuntime *)ud;
	if (!rt || !rt->bt_active_bb) return JCE_BT_FAILURE;
	return jce_blackboard_get_bool(rt->bt_active_bb, "sound.heard", false)
	       ? JCE_BT_SUCCESS : JCE_BT_FAILURE;
}

static JceBtStatus rt_bt_action_in_range(const char *name, void *ud)
{
	(void)name;
	JceRuntime *rt = (JceRuntime *)ud;
	if (!rt || !rt->bt_active_bb) return JCE_BT_FAILURE;
	if (!jce_blackboard_get_bool(rt->bt_active_bb, "target.visible", false))
		return JCE_BT_FAILURE;
	float range = jce_blackboard_get_float(rt->bt_active_bb, "attack.range", 2.0f);
	float dist  = jce_blackboard_get_float(rt->bt_active_bb, "target.distance", 1e9f);
	return (dist <= range) ? JCE_BT_SUCCESS : JCE_BT_FAILURE;
}

/* MoveToTarget hook for the bundled library: drive the currently-ticking
 * agent's NavAgent toward (gx,gz) (Y is navmesh-driven, so ignored here).
 * RUNNING while the path is active, SUCCESS on arrival, FAILURE when the agent
 * has no nav handle or the goal is unreachable. */
JceBtStatus rt_bt_move_to(float gx, float gy, float gz, void *ud)
{
	(void)gy;
	JceRuntime *rt = (JceRuntime *)ud;
	if (!rt || !rt->nav_agents || rt->bt_active_entity == 0)
		return JCE_BT_FAILURE;

	NavAgentEntry *ne = NULL;
	for (int i = 0; i < rt->nav_entry_count; ++i) {
		if (rt->nav_entries[i].entity == rt->bt_active_entity) {
			ne = &rt->nav_entries[i];
			break;
		}
	}
	if (!ne || !jce_nav_agent_valid(ne->handle))
		return JCE_BT_FAILURE;

	/* (Re)issue the destination only when it actually moved, mirroring the
	 * auto_repath gate so we do not re-path every tick. */
	float dx = gx - ne->last_goal_x;
	float dz = gz - ne->last_goal_z;
	if (!ne->has_dest || (dx * dx + dz * dz) > 0.25f) {
		ne->has_dest    = jce_nav_agent_set_destination(rt->nav_agents,
		                                                ne->handle, gx, gz);
		ne->last_goal_x = gx;
		ne->last_goal_z = gz;
		if (!ne->has_dest) return JCE_BT_FAILURE;
	}

	switch (jce_nav_agent_get_status(rt->nav_agents, ne->handle)) {
	case JCE_NAV_AGENT_ARRIVED: return JCE_BT_SUCCESS;
	case JCE_NAV_AGENT_NO_PATH: return JCE_BT_FAILURE;
	default:                    return JCE_BT_RUNNING;
	}
}

/* Register the bundled perception-reading actions + the deterministic node
 * library (Wait / Cooldown / SetBlackboard / … / MoveToTarget) on the BT
 * context once. */
void rt_bt_register_default_actions(JceRuntime *rt)
{
	if (!rt->bt_ctx || rt->bt_actions_registered) return;
	jce_bt_register_action(rt->bt_ctx, "IsTargetVisible", rt_bt_action_is_visible, rt);
	jce_bt_register_action(rt->bt_ctx, "HasTarget",       rt_bt_action_has_target, rt);
	jce_bt_register_action(rt->bt_ctx, "HasHeardSound",   rt_bt_action_has_heard,  rt);
	jce_bt_register_action(rt->bt_ctx, "IsTargetInRange", rt_bt_action_in_range,   rt);
	jce_bt_register_library(rt->bt_ctx);
	rt->bt_actions_registered = true;
}


/* Build the JceScriptHost callback table from this module's bindings and
 * create the runtime's Lua VM (+ editor hot-reload watcher).  Moved verbatim
 * from jce_runtime_create so every rt_script_* callback can stay file-static
 * in this module. */
void rt_script_install_vm(JceRuntime *rt)
{
	JceScriptHost host = {0};
	host.user           = rt;
	host.log            = rt_script_log;
	host.get_position   = rt_script_get_position;
	host.set_position   = rt_script_set_position;
	host.get_rotation   = rt_script_get_rotation;
	host.set_rotation   = rt_script_set_rotation;
	host.get_scale      = rt_script_get_scale;
	host.set_scale      = rt_script_set_scale;
	host.find_with_tag  = rt_script_find_with_tag;
	host.destroy_entity = rt_script_destroy_entity;
	host.spawn          = rt_script_spawn;
	host.move_axis      = rt_script_move_axis;
	host.input_button   = rt_script_input_button;
	host.set_time_scale = rt_script_set_time_scale;
	host.set_paused     = rt_script_set_paused;
	host.shake_camera   = rt_script_shake_camera;
	host.music_set_intensity      = rt_script_music_set_intensity;
	host.music_get_intensity      = rt_script_music_get_intensity;
	host.music_request_transition = rt_script_music_request_transition;
	host.gas_activate   = rt_script_gas_activate;
	host.gas_get        = rt_script_gas_get;
	host.gas_apply      = rt_script_gas_apply;
	host.read_file      = rt_script_read_file;
	host.raycast        = rt_script_raycast;
	host.apply_impulse  = rt_script_apply_impulse;
	host.set_velocity   = rt_script_set_velocity;
	host.get_velocity   = rt_script_get_velocity;
	host.vehicle_set_input = rt_script_vehicle_set_input;
	host.vehicle_get_speed = rt_script_vehicle_get_speed;
	host.get_move          = rt_script_get_move;
	host.anim_set_float   = rt_script_anim_set_float;
	host.anim_set_int     = rt_script_anim_set_int;
	host.anim_set_bool    = rt_script_anim_set_bool;
	host.anim_set_trigger = rt_script_anim_set_trigger;
	host.action_down    = rt_script_action_down;
	host.action_pressed = rt_script_action_pressed;
	host.action_axis    = rt_script_action_axis;
	host.play_sound     = rt_script_play_sound;
	host.ui_get_slider  = rt_script_ui_get_slider;
	host.ui_set_slider  = rt_script_ui_set_slider;
	host.ui_get_toggle  = rt_script_ui_get_toggle;
	host.ui_set_toggle  = rt_script_ui_set_toggle;
	host.ui_set_text    = rt_script_ui_set_text;
	host.send_message   = rt_script_send_message;
	host.broadcast      = rt_script_broadcast;
	host.has_component         = rt_script_has_component;
	host.is_component_enabled  = rt_script_is_component_enabled;
	host.set_component_enabled = rt_script_set_component_enabled;
	host.net_is_server  = rt_script_net_is_server;
	host.net_is_client  = rt_script_net_is_client;
	host.net_spawn      = rt_script_net_spawn;
	host.rpc_send       = rt_script_rpc_send;
	host.particle_burst = rt_script_particle_burst;
	host.particle_set_emitting = rt_script_particle_set_emitting;
	rt->script_vm = jce_script_create(&host);
	/* Script hot-reload watcher (editor dev; inert in shipped).  The
	 * physics contact -> script on_collision bridge is wired per scene in
	 * rt_spawn_scene_state (it depends on the per-scene physics world). */
	if (rt->script_vm)
		rt->script_watcher = jce_file_watcher_create();
}

