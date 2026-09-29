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

#include <jce/middleware/scene/jce_vcam_system.h>

#include "middleware/scene/jce_scene_internal.h"  /* JSON bridges (comp/render get/set) */

#include <jce/middleware/world/jce_environment.h>  /* world-domain script API */
#include <jce/os/platform/jce_input.h>       /* jce.is_key_down */
#include <jce/middleware/script/jce_script_vm.h>  /* per-script language selection */
#include <jce/resource/jce_asset_format.h>        /* the offline script catalog */

/* JceScriptHost::log, and the tag says "[script]" rather than "[lua]".
 *
 * MEASURED (elemental_serenity, four scripted entities, 2026-08-16): a Python
 * script's jce.log line and a Java script's JceEntityScript.log line both
 * arrived tagged "[lua]" —
 *
 *     runtime: [lua] es_fireflies (python) online: 12 fireflies
 *     runtime: [lua] es_campfire (java) online: CampfireLight=ok
 *
 * — because ONE JceScriptHost is shared by every language VM the runtime
 * stands up (rt_script_vm_get passes &rt->script_host to every
 * jce_script_vm_create), and a host callback receives only `user`, which is
 * the JceRuntime.  The callback cannot know which VM called it, and a wrong
 * language name is worse than none: it sends the reader to the wrong file.
 *
 * NOT fixed by naming the language, deliberately.  Doing that means one host
 * copy per language with a per-language `user`, and `user` is cast to
 * JceRuntime * by all ~60 other callbacks in this file — a change with sixty
 * sites and one benefit.  The line that DOES name the language already
 * exists and is printed once per script at load:
 *
 *     script: loaded 'scripts/es_fireflies.py' (python) for entity 641
 *
 * *Enforced by:* nothing, and it needs nothing — the string is a literal with
 * no branch behind it. */
static void rt_script_log(void *user, const char *msg)
{
	(void)user;
	LOG_INFO(LOG_TAG, "[script] %s", msg ? msg : "");
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

static bool rt_script_get_world_position(void *user, JceScriptEntity e,
                                        float out_xyz[3])
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return false;
	if (!jce_scene_has_transform(rt->scene, (JceEntity)e)) return false;
	/* Translation of the composed world matrix.  jce_mat4 is a union of
	 * raw[column][row] (column-major) and col[4]; the translation is
	 * column 3.  Named through col[3] rather than a flat index so a
	 * future layout change breaks the build instead of silently
	 * returning three numbers from the wrong place.
	 *
	 * THIS IS THE MODEL MATRIX, NOT jce_scene_get_world_pose, AND THEY CAN
	 * DISAGREE.  They differ by exactly one thing: JcePivotComponent's offset,
	 * which the world matrix carries (that is what moves the mesh) and the
	 * world POSE deliberately does not (Unity's Transform has no such field).
	 * Physics and the Inspector both use the pose, so for an entity with an
	 * active non-zero pivot -- and a pivot IS serialised, so it reaches a
	 * shipped game -- a script reads where the MESH draws while the collider
	 * sits where the TRANSFORM composes.  Left as it is on purpose: changing
	 * it is a behaviour change to a shipped script API, and the question
	 * underneath it ("should a pivot move the collider?") is a real design
	 * decision, not a bug to fix in passing.  jce_scene_get_pivot_world_position
	 * is the accessor for anyone who wants the pivot itself. */
	jce_mat4 w = jce_scene_get_world_matrix(rt->scene, (JceEntity)e);
	out_xyz[0] = w.col[3].x;
	out_xyz[1] = w.col[3].y;
	out_xyz[2] = w.col[3].z;
	return true;
}

/* jce.line_set_points: replace a LineRenderer's points in one call.
 * See the JceScriptHost member for why this exists (the JSON route is
 * O(n^2) in the point count).  Returns how many points were stored. */
/* The two caps must agree -- see JCE_SCRIPT_LINE_MAX_POINTS.  This TU is the
 * only one that sees both, so the assertion lives here.  C99: a negative-size
 * array typedef, the same idiom the rest of the tree uses. */
typedef char jce_line_cap_matches[
    (JCE_SCRIPT_LINE_MAX_POINTS == JCE_LINE_MAX_POINTS) ? 1 : -1];

static int rt_script_line_set_points(void *user, JceScriptEntity e,
                                     const float *xyz, int count)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !xyz || count <= 0) return 0;
	JceLineRendererComponent *lr =
		jce_scene_get_line_renderer(rt->scene, (JceEntity)e);
	if (!lr) return 0;
	if (count > JCE_LINE_MAX_POINTS) count = JCE_LINE_MAX_POINTS;
	for (int i = 0; i < count; ++i) {
		lr->positions[i][0] = xyz[i * 3 + 0];
		lr->positions[i][1] = xyz[i * 3 + 1];
		lr->positions[i][2] = xyz[i * 3 + 2];
	}
	lr->position_count = count;
	return count;
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

static bool rt_script_set_parent(void *user, JceScriptEntity child,
                                 JceScriptEntity parent, bool preserve_world)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || child == 0) return false;
	return jce_scene_reparent(rt->scene, (JceEntity)child,
	                          parent != 0 ? (JceEntity)parent
	                                      : JCE_ENTITY_INVALID,
	                          preserve_world);
}

static JceScriptEntity rt_script_get_parent(void *user,
                                            JceScriptEntity child)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || child == 0) return 0;
	return (JceScriptEntity)jce_scene_get_parent(rt->scene, (JceEntity)child);
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

static void *rt_read_asset_with_fallback_capped(JceRuntime *rt,
                                                 const char *path,
                                                 uint64_t max_bytes,
                                                 uint64_t *out_size)
{
	char rbuf[1024];
	const char *host_path;
	uint64_t read_size = 0;
	uint64_t total_size = 0;
	void *buf;

	if (out_size) *out_size = 0;
	if (!path || !path[0] || max_bytes == 0) return NULL;
	host_path = rt_resolve_host_path(rt, path, rbuf, sizeof rbuf);
	buf = jce_fs_host_read_capped(host_path, max_bytes,
	                              &read_size, &total_size);
	if (buf) {
		if (total_size > max_bytes || read_size != total_size) {
			jce_fs_buffer_free(buf);
			if (out_size) *out_size = total_size;
			return NULL;
		}
		if (out_size) *out_size = read_size;
		return buf;
	}
	if (rt && rt->pak) {
		const JcePakAsset *asset = jce_pak_find(rt->pak, path);

		if (asset && asset->original_size > 0) {
			if (out_size) *out_size = asset->original_size;
			if (asset->original_size > max_bytes ||
			    asset->original_size > (uint64_t)SIZE_MAX)
				return NULL;
			buf = jce_malloc((size_t)asset->original_size);
			if (buf) {
				size_t n = jce_pak_decompress(asset, buf,
				                              (size_t)asset->original_size);
				if (n == (size_t)asset->original_size)
					return buf;
				jce_free(buf);
			}
		}
	}
	return NULL;
}

static void *rt_script_read_file(void *user, const char *path, uint64_t *out_size)
{
	uint64_t cap = out_size ? *out_size : 0;

	/* A nonzero input is the optional bounded-read convention used by Lua
	 * project-data bindings. Script source loading passes zero and retains the
	 * historical unbounded asset path. */
	if (cap > 0)
		return rt_read_asset_with_fallback_capped((JceRuntime *)user, path,
		                                           cap, out_size);
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

/* ── Level transition (FEATURE 9.4) ───────────────────────────────────
 * jce_runtime_request_scene has existed, documented as "safe to call from
 * gameplay scripts / triggers", with no caller anywhere and no binding in any
 * language -- so a game could not change level.  These two are that seam. */
static bool rt_script_request_scene(void *user, const char *scene_path)
{
	return jce_runtime_request_scene((JceRuntime *)user, scene_path);
}

static bool rt_script_is_transitioning(void *user)
{
	return jce_runtime_is_transitioning((JceRuntime *)user);
}

/* ── AudioSource control (Unity's Play / Stop / isPlaying) ────────────
 * The authored component used to sound once at spawn and never again. */
static bool rt_script_audio_play(void *user, JceScriptEntity e)
{
	return jce_runtime_audio_play((JceRuntime *)user, (uint64_t)e);
}

static bool rt_script_audio_stop(void *user, JceScriptEntity e)
{
	return jce_runtime_audio_stop((JceRuntime *)user, (uint64_t)e);
}

static bool rt_script_audio_is_playing(void *user, JceScriptEntity e)
{
	return jce_runtime_audio_is_playing((JceRuntime *)user, (uint64_t)e);
}

/* ── Save / load (the save system was write-only) ─────────────────────── */
static bool rt_script_save_game(void *user, const char *path)
{
	return jce_runtime_save_to_file((JceRuntime *)user, path);
}

static bool rt_script_load_game(void *user, const char *path)
{
	return jce_runtime_load_from_file((JceRuntime *)user, path);
}

/* ── Spatial queries ─────────────────────────────────────────────────
 *
 * The physics layer answers in BODY HANDLES and gameplay thinks in entities,
 * so the mapping happens here -- rt->bodies[] is the only place it exists.
 * A body the runtime does not know (created outside the spawn walk) is
 * SKIPPED rather than reported as entity 0, which would look like a hit on a
 * real entity. */
static int rt_script_bodies_to_entities(JceRuntime *rt,
                                        const JceBodyHandle *bodies, int n,
                                        JceScriptEntity *out, int max)
{
	int w = 0;
	for (int i = 0; i < n && w < max; ++i) {
		for (int b = 0; b < rt->body_count; ++b) {
			if (rt->bodies[b].body.idx != bodies[i].idx) continue;
			out[w++] = (JceScriptEntity)rt->bodies[b].entity;
			break;
		}
	}
	return w;
}

enum { RT_SCRIPT_QUERY_MAX = 256 };

static int rt_script_overlap_sphere(void *user, float x, float y, float z,
                                    float radius, uint32_t layer_mask,
                                    JceScriptEntity *out, int max)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->physics || !out || max <= 0) return 0;
	JceBodyHandle hits[RT_SCRIPT_QUERY_MAX];
	JceQueryFilter f;
	memset(&f, 0, sizeof f);
	f.layer_mask   = layer_mask ? layer_mask : 0xFFFFFFFFu;
	f.hit_triggers = false;
	const uint32_t n = jce_physics_overlap_sphere(
		rt->physics, jce_v3(x, y, z), radius, f, hits,
		(uint32_t)RT_SCRIPT_QUERY_MAX);
	return rt_script_bodies_to_entities(rt, hits, (int)n, out, max);
}

static int rt_script_overlap_box(void *user, float x, float y, float z,
                                 float hx, float hy, float hz,
                                 uint32_t layer_mask,
                                 JceScriptEntity *out, int max)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->physics || !out || max <= 0) return 0;
	JceBodyHandle hits[RT_SCRIPT_QUERY_MAX];
	JceQueryFilter f;
	memset(&f, 0, sizeof f);
	f.layer_mask   = layer_mask ? layer_mask : 0xFFFFFFFFu;
	f.hit_triggers = false;
	const uint32_t n = jce_physics_overlap_box(
		rt->physics, jce_v3(x, y, z), jce_v3(hx, hy, hz), jce_q_identity(),
		f, hits, (uint32_t)RT_SCRIPT_QUERY_MAX);
	return rt_script_bodies_to_entities(rt, hits, (int)n, out, max);
}

/* ── Adaptive music director script bridge (FEATURE 5.3) ──────────────
 * Act on the runtime's single live JceMusicDirector (built from the scene's
 * MusicTrack component).  All no-op / default when no director exists. */
static void rt_script_music_set_intensity(void *user, float intensity)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (rt && rt->music) jce_music_set_intensity(rt->music, intensity);
}

/* jce.is_key_down(keycode) -> bool.
 *
 * host.is_key_down was the ONE JceScriptHost member with no writer in this
 * file, and the binding it backs had been registered, callable and documented
 * the whole time -- s->host.is_key_down was NULL, so jce_script_bindings.gen.c
 * took its `: false` branch and every query answered "not held", in the
 * editor's Play and in a shipped game alike.
 *
 * Keycodes are JCE_KEY_* from <jce/os/platform/jce_keys.h>: USB HID
 * scancodes, so 4 is A and 26 is W.  A host that supplies no keyboard still
 * answers false -- the same value as before, now because there is nothing to
 * ask rather than because nobody asks.
 */
static bool rt_script_is_key_down(void *user, int keycode)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->input.keyboard) return false;
	return jce_input_key_down(rt->input.keyboard, (JceKey)keycode);
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

/* "Cut to the camera called X".  The vcam system owns the override, not the
 * component, so a script cannot dirty the authored scene by asking for a shot
 * -- see jce_vcam_system.h. */
static int rt_script_vcam_activate(void *user, const char *name)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return 0;
	return jce_vcam_system_set_active_by_name(rt->scene, name) ? 1 : 0;
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

/* The same ray, with the filter the overlap queries have always taken.
 *
 * layer_mask 0 == every layer, the convention rt_script_overlap_sphere uses;
 * the filter is built the same way there, deliberately, because two spellings
 * of "build a JceQueryFilter" is exactly how the ray and the overlaps came to
 * disagree about the mask in the first place. */
static void rt_script_query_filter(JceQueryFilter *f, uint32_t layer_mask,
                                   bool hit_triggers)
{
	memset(f, 0, sizeof *f);
	f->layer_mask   = layer_mask ? layer_mask : 0xFFFFFFFFu;
	f->hit_triggers = hit_triggers;
}

static bool rt_script_raycast_filtered(void *user, const float origin[3],
                                       const float dir[3], float max_dist,
                                       uint32_t layer_mask, bool hit_triggers,
                                       JceScriptRaycastHit *out)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->physics || !origin || !dir || !out) return false;
	JceQueryFilter f;
	rt_script_query_filter(&f, layer_mask, hit_triggers);
	const JceRaycastResult r = jce_physics_raycast_filtered(
		rt->physics, jce_v3(origin[0], origin[1], origin[2]),
		jce_v3(dir[0], dir[1], dir[2]), max_dist, f);
	if (!r.hit) return false;
	out->entity    = (JceScriptEntity)jce_physics_body_get_entity(rt->physics, r.body);
	out->point[0]  = r.point.x;  out->point[1]  = r.point.y;  out->point[2]  = r.point.z;
	out->normal[0] = r.normal.x; out->normal[1] = r.normal.y; out->normal[2] = r.normal.z;
	out->distance  = r.distance;
	return true;
}

/* Every hit along the ray, near->far, as ENTITIES.
 *
 * jce_physics_raycast_all already sorts and already filters; the only work
 * here is mapping bodies to entities, which the runtime is the only thing
 * that can do.  A body the runtime does not know is SKIPPED rather than
 * reported as entity 0 -- rt_script_bodies_to_entities is the one place that
 * rule lives, and reimplementing the loop here would be a second copy of it. */
static int rt_script_raycast_all(void *user, const float origin[3],
                                 const float dir[3], float max_dist,
                                 uint32_t layer_mask, bool hit_triggers,
                                 JceScriptEntity *out, int max)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->physics || !origin || !dir || !out || max <= 0) return 0;
	JceQueryFilter f;
	rt_script_query_filter(&f, layer_mask, hit_triggers);

	JceRaycastResult hits[RT_SCRIPT_QUERY_MAX];
	const uint32_t n = jce_physics_raycast_all(
		rt->physics, jce_v3(origin[0], origin[1], origin[2]),
		jce_v3(dir[0], dir[1], dir[2]), max_dist, f, hits,
		(uint32_t)RT_SCRIPT_QUERY_MAX);

	/* raycast_all hands back RESULTS; the shared mapper takes BODIES, so the
	 * handles are lifted out first rather than duplicating the mapper for a
	 * different element type.  Order is preserved, so the array stays
	 * near->far, which is the only reason a multi-hit ray is more useful than
	 * an overlap. */
	JceBodyHandle bodies[RT_SCRIPT_QUERY_MAX];
	uint32_t      m = 0;
	for (uint32_t i = 0; i < n && i < (uint32_t)RT_SCRIPT_QUERY_MAX; ++i)
		bodies[m++] = hits[i].body;

	return rt_script_bodies_to_entities(rt, bodies, (int)m, out, max);
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

static void rt_script_pointer_delta(void *user, float out_xy[2])
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!out_xy) return;
	out_xy[0] = rt ? rt->input.pointer_dx : 0.0f;
	out_xy[1] = rt ? rt->input.pointer_dy : 0.0f;
}

static float rt_script_pointer_wheel(void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	return rt ? rt->input.pointer_wheel : 0.0f;
}

static const char *rt_script_loc_translate(void *user, const char *key)
{
	(void)user;   /* jce_loc is process-global; user kept for API symmetry */
	return jce_loc_t(key);
}

static const char *rt_script_loc_get_locale(void *user)
{
	(void)user;
	return jce_loc_get_locale();
}

static void rt_script_loc_set_locale(void *user, const char *locale)
{
	(void)user;
	jce_loc_set_locale(locale);
}

static bool rt_script_pointer_button(void *user, int button)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || button < 1 || button > 32) return false;
	return (rt->input.pointer_buttons & (1u << (button - 1))) != 0;
}

static int rt_script_touch_count(void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || rt->input.touch_count < 0) return 0;
	if (rt->input.touch_count > JCE_RUNTIME_MAX_TOUCHES)
		return JCE_RUNTIME_MAX_TOUCHES;
	return rt->input.touch_count;
}

static bool rt_script_touch_get(void *user, int index, uint64_t *id,
								float *x, float *y, float *pressure)
{
	JceRuntime *rt = (JceRuntime *)user;
	const JceRuntimeTouch *touch;
	int count = rt_script_touch_count(user);

	if (!rt || index < 0 || index >= count)
		return false;
	touch = &rt->input.touches[index];
	if (id) *id = touch->id;
	if (x) *x = touch->x;
	if (y) *y = touch->y;
	if (pressure) *pressure = touch->pressure;
	return true;
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
static void rt_script_play_sound_impl(JceRuntime *rt, const char *path,
                                      const float pos[3], float volume,
                                      float min_distance, float max_distance,
                                      float rolloff)
{
	if (!rt || !rt->audio || !path || !path[0]) return;
	JceSound snd = rt_load_sound(rt, path);
	if (snd == JCE_SOUND_INVALID) {
		LOG_WARN(LOG_TAG, "jce.play_sound: cannot load '%s'", path);
		return;
	}
	float vol = (volume > 0.0f) ? volume : 1.0f;
	JceVoice v = jce_audio_play(rt->audio, snd, false, vol, 1.0f);
	if (pos) {
		/* ...unless the AudioListener's spatialize gate is closed.  A
		 * script-started positional sound is still a positional sound, so it
		 * goes through the same gate the AudioSource path does -- otherwise
		 * "turn 3D off" would mean "turn 3D off, except for anything a script
		 * plays", which is the kind of exception nobody discovers until they
		 * hear it. */
		jce_audio_voice_set_3d(rt->audio, v,
		                       rt_voice_should_be_3d(rt->listener_flat,
		                                             true));
		jce_audio_voice_set_position(rt->audio, v, pos[0], pos[1], pos[2]);
		if (min_distance <= 0.0f) min_distance = 1.0f;
		if (max_distance < min_distance) max_distance = min_distance;
		if (rolloff <= 0.0f) rolloff = 1.0f;
		jce_audio_voice_set_attenuation(rt->audio, v,
		                                JCE_AUDIO_ATTEN_INVERSE,
		                                min_distance, max_distance, rolloff);
	} else {
		jce_audio_voice_set_3d(rt->audio, v, false);
	}
}

static void rt_script_play_sound(void *user, const char *path,
                                 const float pos[3], float volume)
{
	rt_script_play_sound_impl((JceRuntime *)user, path, pos, volume,
	                          1.0f, 25.0f, 1.0f);
}

static void rt_script_play_sound_spatial(void *user, const char *path,
                                         const float pos[3], float volume,
                                         float min_distance,
                                         float max_distance, float rolloff)
{
	rt_script_play_sound_impl((JceRuntime *)user, path, pos, volume,
	                          min_distance, max_distance, rolloff);
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

/* jce.ui_get_progress / ui_set_progress: the live UIProgressBar's value.
 *
 * IN THE COMPONENT'S OWN [min_value, max_value] RANGE, not 0..1 -- the bar
 * draws clamp((value-min)/(max-min)), so 0..1 is the FILL and not the value.
 * Mirrors ui_get_slider, whose component carries the same pair.
 *
 * The set CLAMPS rather than refusing, because the draw already clamps: an
 * unclamped store would make the component and the picture disagree, and a
 * script reading back what it just wrote would get a number the bar is not
 * showing.  A degenerate range (max <= min) is left alone -- the draw returns
 * a zero fill for it, and inventing a clamp order there would be this file
 * deciding something the component has not. */
static bool rt_script_ui_get_progress(void *user, JceScriptEntity e, float *out)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !out) return false;
	JceUIProgressBarComponent *pb =
		jce_scene_get_ui_progress_bar(rt->scene, (JceEntity)e);
	if (!pb) return false;
	*out = pb->value;
	return true;
}

static void rt_script_ui_set_progress(void *user, JceScriptEntity e, float v)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return;
	JceUIProgressBarComponent *pb =
		jce_scene_get_ui_progress_bar(rt->scene, (JceEntity)e);
	if (!pb) return;
	if (pb->max_value > pb->min_value) {
		if (v < pb->min_value) v = pb->min_value;
		if (v > pb->max_value) v = pb->max_value;
	}
	pb->value = v;   /* MUTABLE accessor: write the new value back in place */
}

/* jce.ui_get_dropdown / ui_set_dropdown: the live UIDropdown's selection.
 *
 * The INDEX, not the label: a script that wants the text reads options[] via
 * comp_get_json, and an accessor that returned the label would make the
 * common case (branch on which option) a string compare. */
static bool rt_script_ui_get_dropdown(void *user, JceScriptEntity e, int *out)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !out) return false;
	JceUIDropdownComponent *dd =
		jce_scene_get_ui_dropdown(rt->scene, (JceEntity)e);
	if (!dd) return false;
	*out = dd->selected_index;
	return true;
}

static void rt_script_ui_set_dropdown(void *user, JceScriptEntity e, int index)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return;
	JceUIDropdownComponent *dd =
		jce_scene_get_ui_dropdown(rt->scene, (JceEntity)e);
	if (!dd) return;
	/* CLAMPED, not refused -- the same reason ui_set_progress clamps: the
	 * draw already clamps, so storing an out-of-range index would make the
	 * component and the picture disagree.  The scene loader clamps the same
	 * way (jce_scene_components_ui.c), so a value written by a script and a
	 * value written by a scene file land in the same place. */
	if (index < 0) index = 0;
	if (dd->option_count > 0 && index >= dd->option_count)
		index = dd->option_count - 1;
	dd->selected_index = index;
}

/* jce.ui_get_input_text / ui_set_input_text: the live UIInputField's text.
 *
 * get returns a POINTER INTO THE COMPONENT, valid until the next mutation of
 * that entity -- the same contract tr() and get_locale() have, and the reason
 * this is a plain value_return rather than an owned_string_release: there is
 * nothing to free, and a heap copy would make reading a name allocate. */
static const char *rt_script_ui_get_input_text(void *user, JceScriptEntity e)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return NULL;
	JceUIInputFieldComponent *f =
		jce_scene_get_ui_input_field(rt->scene, (JceEntity)e);
	return f ? f->text : NULL;
}

static void rt_script_ui_set_input_text(void *user, JceScriptEntity e,
                                        const char *text)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !text) return;
	JceUIInputFieldComponent *f =
		jce_scene_get_ui_input_field(rt->scene, (JceEntity)e);
	if (!f) return;
	size_t n = strlen(text);
	if (n >= sizeof f->text) {
		/* SAY IT, for the reason ui_set_text gives at length: the
		 * truncation is right (the field is fixed) and dropping the tail
		 * in silence is how a script ships a form value nobody can read
		 * with nothing anywhere saying why. */
		LOG_WARN(LOG_TAG,
		         "jce.ui_set_input_text: truncated %zu bytes to %zu for "
		         "entity %llu -- UIInputField.text is a fixed field",
		         n, sizeof f->text - 1u, (unsigned long long)e);
		n = sizeof f->text - 1u;
	}
	memcpy(f->text, text, n);
	f->text[n] = 0;
	/* char_limit is the AUTHORED cap and the canvas enforces it on typed
	 * input; a script write honours it too, or the two paths would disagree
	 * about what the field can hold. */
	if (f->char_limit > 0 && (size_t)f->char_limit < n)
		f->text[f->char_limit] = 0;
}

/* jce.ui_get_scroll / ui_set_scroll: the live UIScrollView's offset.
 *
 * In REFERENCE UNITS, which is what the component stores and what the wheel
 * path clamps -- not device px.  A script that moved a list by device px
 * would move it a different distance on every DPI. */
static bool rt_script_ui_get_scroll(void *user, JceScriptEntity e,
                                    float out_xy[2])
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !out_xy) return false;
	JceUIScrollViewComponent *sv =
		jce_scene_get_ui_scroll_view(rt->scene, (JceEntity)e);
	if (!sv) return false;
	out_xy[0] = sv->scroll_position[0];
	out_xy[1] = sv->scroll_position[1];
	return true;
}

/* ── World domain: the clock and the weather ────────────────────────────
 *
 * The WRITER for the six world_* host members.  Declaring them without this
 * would ship the exact shape this audit keeps finding: a carrier that lands
 * before its writer, where every script call returns a plausible default and
 * nothing says the feature is inert.
 *
 * All six are scene-scoped and take no handle: a script's world is the
 * runtime's current scene, resolved here rather than handed out as a raw
 * engine pointer.
 */
static float rt_script_world_get_hour(void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return 0.0f;
	/* The LIVE hour, not JceSceneRenderingSettings::tod_hour: that field is
	 * the authored seed, and a script reading it would get the level's
	 * start-of-day forever while the sky moved. */
	return jce_scene_environment_hour(rt->scene);
}

static void rt_script_world_set_hour(void *user, float hour)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return;
	/* Moves the live clock only; the authored seed is untouched, so
	 * reloading the scene still starts where the designer set it. */
	jce_scene_environment_set_hour(rt->scene, hour);
}

static bool rt_script_world_is_daytime(void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return false;
	const JceEnvironmentState *env = jce_scene_environment(rt->scene);
	return env ? jce_environment_is_daytime(env) : false;
}

static int rt_script_world_get_weather(void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return 0;
	const JceSceneRenderingSettings *rs =
		jce_scene_get_rendering_settings(rt->scene);
	return rs ? rs->weather_type : 0;
}

static float rt_script_world_get_weather_intensity(void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return 0.0f;
	const JceSceneRenderingSettings *rs =
		jce_scene_get_rendering_settings(rt->scene);
	return rs ? rs->weather_intensity : 0.0f;
}

static float rt_script_world_get_wind_speed(void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return 0.0f;
	const JceEnvironmentState *env = jce_scene_environment(rt->scene);
	/* Instantaneous: sustained + this moment's gust.  The same number the
	 * ocean spectrum and the vegetation shader read, so a script cannot
	 * disagree with what is on screen. */
	return env ? jce_environment_wind_speed_now(env) : 0.0f;
}

static void rt_script_ui_set_scroll(void *user, JceScriptEntity e,
                                    float x, float y)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return;
	JceUIScrollViewComponent *sv =
		jce_scene_get_ui_scroll_view(rt->scene, (JceEntity)e);
	if (!sv) return;
	/* Clamped to [0, max(0, content - viewport)] per axis, and a DISABLED
	 * axis is pinned to 0 -- the same rule the wheel path applies.  The
	 * viewport extent is not known here (it is resolved during layout), so
	 * this clamps against content_size alone and the canvas re-clamps on the
	 * next render; a script therefore cannot push the offset somewhere the
	 * wheel could not, which is the property that matters. */
	if (!sv->horizontal) x = 0.0f;
	if (!sv->vertical)   y = 0.0f;
	if (x < 0.0f) x = 0.0f;
	if (y < 0.0f) y = 0.0f;
	if (sv->content_size[0] > 0.0f && x > sv->content_size[0])
		x = sv->content_size[0];
	if (sv->content_size[1] > 0.0f && y > sv->content_size[1])
		y = sv->content_size[1];
	sv->scroll_position[0] = x;
	sv->scroll_position[1] = y;
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
	if (n >= sizeof ut->text) {
		/* **Say it.** The truncation itself is right -- the component's
		 * text is a fixed field and overrunning it is not an option --
		 * but silently dropping the tail is how a script ships a HUD
		 * line nobody can read with nothing anywhere saying why.
		 * space/ hit exactly that: its capture banner runs to 583 bytes
		 * once six rows are live, and the demo only found out by
		 * mirroring this function's private cap into its own Lua constant
		 * and splitting on row boundaries by hand.  A layout gate checks
		 * geometry and cannot see a byte count.
		 *
		 * Rate-limited to the ONSET of truncation: the offending text is
		 * usually rewritten every frame (space/ logged 2510 in one take),
		 * and a log that floods is a log nobody reads. */
		if (!rt->ui_text_clipped_warned) {
			rt->ui_text_clipped_warned = true;
			LOG_WARN(LOG_TAG,
			         "ui_set_text: entity %u text truncated -- %u bytes "
			         "into a %u-byte field, the tail is dropped",
			         (unsigned)e, (unsigned)n, (unsigned)sizeof ut->text);
		}
		n = sizeof ut->text - 1;
	}
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
	if (!rt || !ev) return;
	if (ev->type != JCE_CONTACT_BEGIN) return;   /* fire once per contact pair */
	/* JceFractureComponent.break_impulse: the hit that breaks it.  Fracture
	 * was API-only (jce_runtime_fracture_entity), so a wall could be hit at
	 * any speed and never break unless a script noticed and asked for it --
	 * the threshold was authored, serialised, in the Inspector, and compared
	 * against nothing, because the contact event carried no impulse at all.
	 *
	 * <= 0 means "never breaks by impact", which is what a zeroed component
	 * carries and therefore what every existing scene means.  Checked on
	 * BOTH bodies: either side of a collision can be the fracturable one.
	 */
	if (rt->scene && ev->applied_impulse > 0.0f) {
		const uint64_t pair[2] = { ev->entity_a, ev->entity_b };
		for (int fp = 0; fp < 2; ++fp) {
			if (!pair[fp]) continue;
			JceFractureComponent *fx =
				jce_scene_get_fracture(rt->scene, (JceEntity)pair[fp]);
			if (!fx || !fx->enabled) continue;
			if (fx->break_impulse <= 0.0f) continue;
			if (ev->applied_impulse < fx->break_impulse) continue;
			LOG_INFO(LOG_TAG,
			         "fracture: entity %llu broke on impulse %.2f (>= %.2f)",
			         (unsigned long long)pair[fp],
			         (double)ev->applied_impulse, (double)fx->break_impulse);
			jce_runtime_fracture_entity(rt, pair[fp]);
		}
	}

	for (int i = 0; i < rt->script_count; ++i) {
		struct ScriptEntry *se = &rt->scripts[i];
		if (!se->active) continue;
		if ((uint64_t)se->entity == ev->entity_a && ev->entity_b != 0)
			rt_script_ref_collision(se->ref, (JceScriptEntity)ev->entity_b);
		else if ((uint64_t)se->entity == ev->entity_b && ev->entity_a != 0)
			rt_script_ref_collision(se->ref, (JceScriptEntity)ev->entity_a);
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
	if (!rt || !msg) return;
	for (int i = 0; i < rt->script_count; ++i) {
		struct ScriptEntry *se = &rt->scripts[i];
		if (!se->active) continue;
		if ((JceScriptEntity)se->entity == target) {
			rt_script_ref_message(se->ref, msg, number_arg, str_arg);
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
	if (!rt || !msg) return;
	for (int i = 0; i < rt->script_count; ++i) {
		struct ScriptEntry *se = &rt->scripts[i];
		if (!se->active) continue;
		rt_script_ref_message(se->ref, msg, number_arg, str_arg);
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
	if (jce_scene_has_comp(rt->scene, (JceEntity)e, cid)) {
		/* Attribute the write before performing it: a script that
		 * re-asserts the same value every frame changes nothing, so
		 * the editor cannot infer ownership from state transitions. */
		jce_scene_mark_comp_script_driven(rt->scene, (JceEntity)e, cid);
		jce_scene_set_comp_enabled(rt->scene, (JceEntity)e, cid, on);
	}
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

/* jce.particle_set_color(entity, r, g, b): retint the entity emitter's newly
 * spawned particles (no-op when the entity has no emitter). */
static void rt_script_particle_set_color(void *user, JceScriptEntity e,
                                         float r, float g, float b)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return;
	jce_scene_particle_set_color(rt->scene, (JceEntity)e, r, g, b);
}

/* ── Scene-driver host (editor-Play/runtime logic parity) ─────────────────
 * Thin passthroughs to the scene's query + JSON-reflection surfaces so a
 * scene-bound script can own look/season/weather logic that used to need
 * app exe code.  All are safe no-ops without a scene. */

static int rt_script_find_by_name(void *user, const char *name,
                                  JceScriptEntity *out, int max)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !name || !out || max <= 0) return 0;
	/* JceScriptEntity is the raw entity id — query directly into a small
	 * local then widen (the two types share representation but not size
	 * guarantees). */
	JceEntity tmp[64];
	int want = max < 64 ? max : 64;
	int n = jce_scene_query_by_name(rt->scene, name, tmp, want);
	for (int i = 0; i < n; i++) out[i] = (JceScriptEntity)tmp[i];
	return n;
}

typedef struct {
	const char      *prefix;
	size_t           plen;
	JceScriptEntity *out;
	int              max;
	int              n;
} RtFindPrefixCtx;

static void rt_find_prefix_cb(JceScene *s, JceEntity e, void *ud)
{
	RtFindPrefixCtx *c = (RtFindPrefixCtx *)ud;
	if (c->n >= c->max) return;
	const char *nm = jce_scene_entity_name(s, e);
	if (nm && strncmp(nm, c->prefix, c->plen) == 0)
		c->out[c->n++] = (JceScriptEntity)e;
}

static int rt_script_find_by_prefix(void *user, const char *prefix,
                                    JceScriptEntity *out, int max)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !prefix || !out || max <= 0) return 0;
	RtFindPrefixCtx c = { prefix, strlen(prefix), out, max, 0 };
	jce_scene_each_entity(rt->scene, rt_find_prefix_cb, &c);
	return c.n;
}

static char *rt_script_comp_get_json(void *user, JceScriptEntity e,
                                     const char *type)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return NULL;
	return jce_scene_component_to_json(rt->scene, (JceEntity)e, type);
}

static bool rt_script_comp_set_json(void *user, JceScriptEntity e,
                                    const char *type, const char *json)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return false;
	return jce_scene_component_apply_json(rt->scene, (JceEntity)e, type, json);
}

static char *rt_script_render_get_json(void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return NULL;
	return jce_scene_rendering_to_json(rt->scene);
}

static bool rt_script_render_set_json(void *user, const char *json)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return false;
	return jce_scene_rendering_apply_json(rt->scene, json);
}

static void rt_script_json_free(void *user, char *s)
{
	(void)user;
	jce_scene_json_free(s);
}

/* jce.audio_set_volume(entity, vol): update the live voice AND its authored
 * base (the occlusion pass rescales from base_volume each frame — see
 * jce_rt_audio.c — so writing only the live voice would be overwritten). */
static void rt_script_audio_set_volume(void *user, JceScriptEntity e, float vol)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->audio) return;
	if (vol < 0.0f) vol = 0.0f;
	for (int i = 0; i < rt->voice_count; ++i) {
		VoiceEntry *ve = &rt->voices[i];
		if (ve->entity != (JceEntity)e) continue;
		ve->base_volume = vol;
		jce_audio_set_volume(rt->audio, ve->voice, vol);
	}
}

/* File-watcher callback: a script's source changed on disk → hot-reload it. */
void rt_on_script_changed(const char *path, void *user)
{
	jce_runtime_reload_script((JceRuntime *)user, path);
}

/* Where a save with this id and slot lands.
 *
 * Its own function so it can be exercised without a live runtime: this is
 * the whole observable effect of JceSavePointComponent.slot, which was
 * authored, serialised and read by nothing -- every save point in a scene
 * wrote "<save_id>.jsnp" however many slots the designer laid out.
 *
 * slot < 0 is the PARSER'S DEFAULT and keeps the historical single-file
 * name, so nothing already authored moves.  slot 0 is an authored slot and
 * deliberately does NOT collapse onto it. */
bool rt_save_path(char *out, size_t cap, const char *dir,
                  const char *save_id, int slot)
{
	if (!out || cap == 0 || !dir || !save_id) return false;
	int n = (slot >= 0)
	      ? snprintf(out, cap, "%s/%s.slot%d.jsnp", dir, save_id, slot)
	      : snprintf(out, cap, "%s/%s.jsnp", dir, save_id);
	return n > 0 && (size_t)n < cap;
}

/* Write a session snapshot to "<saves_dir>/<save_id>.jsnp" through the
 * runtime registry, creating the saves directory on demand (P2-save-snapshot).
 * No-op (returns false) when no saves directory was configured. */
bool rt_perform_save(JceRuntime *rt, const char *save_id)
{
	return rt_perform_save_slot(rt, save_id, -1);
}

bool rt_perform_save_slot(JceRuntime *rt, const char *save_id, int slot)
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
	if (!rt_save_path(path, sizeof path, rt->saves_dir, save_id, slot))
		return false;

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


/* ── PER-SCRIPT LANGUAGE SELECTION ────────────────────────────────────────
 *
 * bob.lua and turret.py sit in one scene and each runs in its own VM.  The
 * path decides, through the extension claims backends make for themselves
 * (jce_script_vm.h, REGISTERING A LANGUAGE) — the engine holds no list of
 * languages and no list of extensions, so a sixth backend needs no edit here.
 *
 * JCE_SCRIPT_LANGUAGE IS NOW A GLOBAL OVERRIDE, and that is the only meaning
 * under which its name stays true once selection is per script: set it and
 * EVERY script in the process runs in that one language whatever its
 * extension — which is exactly the old whole-process behaviour, kept because
 * it is what a cross-language differential needs (run this scene under
 * python) and because silently reinterpreting the variable as "a default for
 * unclaimed extensions" would make `JCE_SCRIPT_LANGUAGE=lua` load turret.py
 * as a Lua chunk.  Unset (the normal case) selects per script.
 *
 * THERE IS NO FALLBACK, AND THAT IS THE WHOLE POINT.  Nothing turns an
 * unresolved path into Lua.  Falling back would load a `.py` as a Lua chunk
 * and report a syntax error against the script instead of the missing
 * backend: the exact shape of "nothing compares equal to nothing" that a
 * cross-language differential exists to catch.  An unresolved path yields no
 * instance, that entity's script does not run, and the failure is logged with
 * both registries printed. */
static bool rt_script_override(const char **out_lang)
{
	const char *lang = getenv("JCE_SCRIPT_LANGUAGE");
	if (!lang || !lang[0]) return false;
	*out_lang = lang;
	return true;
}

/* Both registries, rendered for a diagnostic.
 *
 * THE BUFFERS ARE SMALL ON PURPOSE.  jce_log's ring carries 512 bytes per
 * message (engine/src/os/core/jce_log_ring.h) and silently truncates past it,
 * so a single long explanatory line loses its TAIL — which is where the
 * actionable half of any explanation ends up.  The refusals below therefore
 * emit several short lines instead of one long one, and these two buffers are
 * sized so that each line plus its prefix still fits.  Measured after the
 * change: the last sentence of the last line is present in the log. */
#define RT_SCRIPT_LANGS_CAP 192
#define RT_SCRIPT_EXTS_CAP  256

static void rt_script_registries(char *langs, size_t langs_cap,
                                 char *exts,  size_t exts_cap)
{
	size_t used = 0;
	int    i, n;

	langs[0] = '\0';
	for (i = 0, n = jce_script_vm_count(); i < n; ++i) {
		const char *nm = jce_script_vm_language_at(i);
		if (!nm) continue;
		int w = snprintf(langs + used, langs_cap - used,
		                 used ? ", %s" : "%s", nm);
		if (w <= 0 || (size_t)w >= langs_cap - used) break;
		used += (size_t)w;
	}
	if (!langs[0]) snprintf(langs, langs_cap, "<none>");

	used = 0;
	exts[0] = '\0';
	for (i = 0, n = jce_script_vm_extension_count(); i < n; ++i) {
		const char *e = jce_script_vm_extension_at(i);
		const char *l = jce_script_vm_extension_language_at(i);
		if (!e || !l) continue;
		int w = snprintf(exts + used, exts_cap - used,
		                 used ? ", .%s -> %s" : ".%s -> %s", e, l);
		if (w <= 0 || (size_t)w >= exts_cap - used) break;
		used += (size_t)w;
	}
	if (!exts[0]) snprintf(exts, exts_cap, "<none>");
}

/* Both live registries, as evidence under a refusal.  Emitted as separate
 * lines so none of it is lost to the 512-byte message limit.  The PRESCRIPTION
 * is not printed here — each caller knows which failure it hit and says so
 * itself, because the three are not the same problem. */
static void rt_script_log_registry_evidence(void)
{
	char langs[RT_SCRIPT_LANGS_CAP];
	char exts[RT_SCRIPT_EXTS_CAP];
	rt_script_registries(langs, sizeof langs, exts, sizeof exts);
	LOG_ERROR(LOG_TAG, "  registered languages: %s", langs);
	LOG_ERROR(LOG_TAG, "  claimed extensions  : %s", exts);
}

/* The language `path` must run in, or NULL with the reason logged.
 *
 * ══ WHY THE OFFLINE CATALOG IS CONSULTED AFTER THE RUNTIME REGISTRY FAILS ══
 *
 * "this engine has no such language" and "this language's backend was not
 * linked into this executable" have COMPLETELY different fixes — write a whole
 * JceScriptVM, versus add one library and one register() call — and until this
 * function was changed, both printed the same sentence: "no script VM claims
 * its extension", followed by the live registry and a paragraph asking the
 * READER to work out which case they were in.
 *
 * They do not have to.  jce_asset_script_language_from_ext() is the OFFLINE
 * catalog (engine/src/resource/jce_asset_ext.c) and it is deliberately
 * independent of which backends were built: the cooker with no Python linked
 * still has to know turret.py is Python in order to pack it for a runtime that
 * does.  So it answers "python" here even in an executable that contains no
 * Python at all, and that is exactly the fact that separates the two cases.
 *
 * MEASURED (elemental_serenity, packaged PAK-only build, python register()
 * suppressed to model the un-linked exe, 2026-08-16).  Before:
 *
 *     script 'scripts/es_fireflies.py': no script VM claims its extension
 *       registered languages: java, lua, cpp
 *
 * The word "python" never appears.  A reader who does not already know this
 * engine HAS a Python backend reads "java, lua, cpp", concludes case (a), and
 * sets out to write a JceScriptVM — the expensive wrong fix for a missing
 * target_link_libraries.
 *
 * The `.escpp` case is why the else-branch is worded as it is: a project may
 * claim its OWN extension at runtime for an already-registered language, and
 * such an extension is legitimately absent from the catalog.  So "not in the
 * catalog" is reported as the two possibilities it really is, not as a verdict.
 *
 * *Enforced by:* tests/os/resource/test_jce_asset_ext.c ::
 * test_the_catalog_answers_without_any_backend_linked — that binary links
 * `jce_core jce_resource` and NO script VM at all, so it is a process in which
 * the catalog CANNOT be answering from the live registry; it pins "python" for
 * .py, "java" for .class, and NULL for both an unimplemented language and a
 * project-private extension, which are the three answers this branch reads. */
static const char *rt_script_language_for(const char *path)
{
	const char *lang = NULL;
	const char *catalog_lang;

	if (rt_script_override(&lang)) return lang;   /* checked at create time */

	lang = jce_script_vm_language_for_path(path);
	if (lang) return lang;

	catalog_lang = jce_asset_script_language_from_ext(path);
	if (catalog_lang) {
		LOG_ERROR(LOG_TAG,
		          "script '%s' is %s, and NO %s BACKEND IS IN THIS "
		          "EXECUTABLE — this entity's script will NOT run, and it is "
		          "NOT run as Lua.",
		          path, catalog_lang, catalog_lang);
		rt_script_log_registry_evidence();
		LOG_ERROR(LOG_TAG,
		          "  FIX: link scripting/%s into this target and call "
		          "jce_script_vm_%s_register() at startup, BEFORE the first "
		          "scene loads — Script components are instantiated inside "
		          "jce_runtime_create(). You do NOT need to write a VM: this "
		          "engine implements %s already.",
		          catalog_lang, catalog_lang, catalog_lang);
	} else {
		LOG_ERROR(LOG_TAG,
		          "script '%s': no language claims its extension — this "
		          "entity's script will NOT run, and it is NOT run as Lua.",
		          path);
		rt_script_log_registry_evidence();
		LOG_ERROR(LOG_TAG,
		          "  FIX: the extension is in neither the live registry above "
		          "nor the offline catalog (engine/src/resource/jce_asset_ext.c), "
		          "so either no backend implements it — write a JceScriptVM, see "
		          "engine/include/jce/middleware/script/jce_script_vm.h — or it "
		          "is a project-private extension whose "
		          "jce_script_vm_register_extension() call did not run.");
	}
	return NULL;
}

/* The already-created VM for `language`, or NULL. */
static JceScript *rt_script_vm_existing(JceRuntime *rt, const char *language)
{
	int i;
	if (!rt || !language) return NULL;
	for (i = 0; i < rt->script_lang_count; ++i)
		if (strcmp(rt->script_langs[i].language, language) == 0)
			return rt->script_langs[i].vm;
	return NULL;
}

/* The VM for `language`, standing it up on first use.  NULL (logged) when the
 * language is not registered or its create_sized refuses. */
static JceScript *rt_script_vm_get(JceRuntime *rt, const char *language)
{
	JceScript *s = rt_script_vm_existing(rt, language);
	if (s) return s;

	if (rt->script_lang_count >= JCE_SCRIPT_VM_MAX) {
		LOG_ERROR(LOG_TAG,
		          "script VM '%s' not created: the runtime already holds %d "
		          "languages", language, rt->script_lang_count);
		return NULL;
	}
	if (strlen(language) >= (size_t)JCE_SCRIPT_VM_LANGUAGE_MAX) {
		LOG_ERROR(LOG_TAG, "script VM '%s' not created: name too long",
		          language);
		return NULL;
	}

	s = jce_script_vm_create(language, &rt->script_host,
	                         sizeof rt->script_host);
	if (!s) {
		LOG_ERROR(LOG_TAG,
		          "script VM '%s' could not be created — every script in that "
		          "language will NOT run. If '%s' IS listed below, its "
		          "create_sized refused and logged why above this line.",
		          language, language);
		rt_script_log_registry_evidence();
		return NULL;
	}

	rt->script_langs[rt->script_lang_count].vm = s;
	snprintf(rt->script_langs[rt->script_lang_count].language,
	         sizeof rt->script_langs[0].language, "%s", language);
	++rt->script_lang_count;
	LOG_INFO(LOG_TAG, "script VM created for language '%s'", language);
	return s;
}

RtScriptRef rt_script_instantiate(JceRuntime *rt, const char *path,
                                  JceEntity owner)
{
	RtScriptRef ref = {NULL, 0};
	const char *language;
	JceScript  *vm;

	if (!rt || !rt->script_enabled || !path || !path[0]) return ref;

	language = rt_script_language_for(path);   /* logs its own refusal */
	if (!language) return ref;
	vm = rt_script_vm_get(rt, language);       /* logs its own refusal */
	if (!vm) return ref;

	/* THE ONLY PLACE EITHER HALF OF AN RtScriptRef IS WRITTEN.  Both come
	 * from this one call, so the pair cannot be mismatched without someone
	 * hand-building the struct — see RtScriptRef in jce_rt_internal.h. */
	ref.vm   = vm;
	ref.inst = jce_script_instantiate(vm, path, (JceScriptEntity)owner);
	if (!ref.inst) ref.vm = NULL;
	return ref;
}

JceScript *rt_script_vm_for_path_existing(JceRuntime *rt, const char *path)
{
	const char *lang = NULL;
	if (!rt || !rt->script_enabled || !path || !path[0]) return NULL;
	if (!rt_script_override(&lang))
		lang = jce_script_vm_language_for_path(path);
	return lang ? rt_script_vm_existing(rt, lang) : NULL;
}

/* ── Global (non-instance) handlers ───────────────────────────────────────
 *
 * A UIButton's on_click / a sequencer EVENT key names a GLOBAL function, not
 * an instance, so there is no ref to route it with.  With one VM that was not
 * a question; with several the handler may live in any of them, so each live
 * language is asked in creation order and the first that HANDLED it wins.
 *
 * The three slots return false for "no such global" — the header calls
 * call_named_num and call_named_str THE TWO SLOTS THAT FAIL SILENTLY for
 * exactly this reason — so "false" is precisely "not mine, ask the next one",
 * which is what makes this loop correct rather than a guess.  With a single
 * Lua VM the loop runs once and the result is identical to the old direct
 * call. */
bool rt_script_call_named(JceRuntime *rt, const char *fn_name,
                          JceScriptEntity arg_entity)
{
	int i;
	if (!rt || !fn_name || !fn_name[0]) return false;
	for (i = 0; i < rt->script_lang_count; ++i)
		if (jce_script_call_named(rt->script_langs[i].vm, fn_name, arg_entity))
			return true;
	return false;
}

bool rt_script_call_named_num(JceRuntime *rt, const char *fn_name,
                              JceScriptEntity arg_entity, double value)
{
	int i;
	if (!rt || !fn_name || !fn_name[0]) return false;
	for (i = 0; i < rt->script_lang_count; ++i)
		if (jce_script_call_named_num(rt->script_langs[i].vm, fn_name,
		                              arg_entity, value))
			return true;
	return false;
}

bool rt_script_call_named_str(JceRuntime *rt, const char *fn_name,
                              JceScriptEntity arg_entity, const char *str)
{
	int i;
	if (!rt || !fn_name || !fn_name[0]) return false;
	for (i = 0; i < rt->script_lang_count; ++i)
		if (jce_script_call_named_str(rt->script_langs[i].vm, fn_name,
		                              arg_entity, str))
			return true;
	return false;
}

void rt_script_destroy_vms(JceRuntime *rt)
{
	int i;
	if (!rt) return;
	for (i = 0; i < rt->script_lang_count; ++i) {
		jce_script_destroy(rt->script_langs[i].vm);
		rt->script_langs[i].vm = NULL;
	}
	rt->script_lang_count = 0;
}

/* THE TWO ENUMS MUST AGREE, and they are declared in two headers that cannot
 * include each other: JceScriptParamKind lives in the scene header and
 * JceScriptParamValue.kind in the script header, which may not depend on the
 * scene.  This TU is the only one that sees both.  A renumbering that drifts
 * would hand every script a correctly-shaped value of the wrong kind -- a
 * TEXT read as an ENTITY, silently.  C99 negative-size array, the idiom the
 * rest of this file already uses for JCE_SCRIPT_LINE_MAX_POINTS. */
typedef char jce_script_param_kinds_match[
    ((int)JCE_SCRIPT_PARAM_NUMBER == 0 && (int)JCE_SCRIPT_PARAM_BOOL   == 1 &&
     (int)JCE_SCRIPT_PARAM_TEXT   == 2 && (int)JCE_SCRIPT_PARAM_ENTITY == 3)
        ? 1 : -1];

/* Read an authored parameter off the entity's script component.
 *
 * FALSE FOR ALL THREE ABSENCES -- no component, no such name, no name at all
 * -- because a script cannot act differently on them and should not have to:
 * every one means "the author did not give me this".  the out parameters are left untouched
 * so the caller's default survives, which is what makes
 *
 *     local speed = jce.get_param(self.entity, "speed") or 3.0
 *
 * read the way an author expects. */
static bool rt_script_get_script_param(void *user, JceScriptEntity e,
                                       const char *name,
                                       int *out_kind, double *out_number,
                                       JceScriptEntity *out_entity)
{
	JceRuntime *rt = (JceRuntime *)user;
	JceScriptComponent *sc;
	int i;

	if (!rt || !rt->scene || !name || name[0] == '\0') return false;
	if (!out_kind || !out_number || !out_entity) return false;
	sc = jce_scene_get_script(rt->scene, (JceEntity)e);
	if (!sc) return false;

	int n = sc->param_count;
	if (n < 0) n = 0;
	if (n > JCE_SCRIPT_PARAM_MAX) n = JCE_SCRIPT_PARAM_MAX;

	for (i = 0; i < n; ++i) {
		const JceScriptParam *pm = &sc->params[i];
		if (pm->name[0] == '\0') continue;       /* an unused slot */
		if (strcmp(pm->name, name) != 0) continue;

		*out_kind   = (int)pm->kind;
		*out_number = 0.0;
		*out_entity = 0;
		switch ((JceScriptParamKind)pm->kind) {
		case JCE_SCRIPT_PARAM_BOOL:
			/* Normalised to 0 or 1 here rather than passed through: an
			 * author's 0.5 in a bool slot must not reach a script as a
			 * number that is neither true nor false. */
			*out_number = (pm->number != 0.0f) ? 1.0 : 0.0;
			break;
		case JCE_SCRIPT_PARAM_TEXT:
			/* The VALUE is not carried here -- see get_script_param_text.
			 * `kind` still is, so a script that asked for the wrong
			 * accessor can tell the parameter EXISTS and is text rather
			 * than concluding the author never set it. */
			break;
		case JCE_SCRIPT_PARAM_ENTITY:
			*out_entity = (JceScriptEntity)pm->entity;
			break;
		case JCE_SCRIPT_PARAM_NUMBER:
		default:
			*out_number = (double)pm->number;
			break;
		}
		return true;
	}
	return false;
}

/* The TEXT half.  Separate because a POD out-struct is flattened to scalars
 * by the binding generator, so a char[64] inside JceScriptParamValue would
 * have become sixty-four out-parameters.  Shaped like ui_get_input_text: a
 * pointer into the component, "" when there is nothing to return. */
static const char *rt_script_get_script_param_text(void *user,
                                                   JceScriptEntity e,
                                                   const char *name)
{
	JceRuntime *rt = (JceRuntime *)user;
	JceScriptComponent *sc;
	int i;

	if (!rt || !rt->scene || !name || name[0] == '\0') return "";
	sc = jce_scene_get_script(rt->scene, (JceEntity)e);
	if (!sc) return "";

	int n = sc->param_count;
	if (n < 0) n = 0;
	if (n > JCE_SCRIPT_PARAM_MAX) n = JCE_SCRIPT_PARAM_MAX;

	for (i = 0; i < n; ++i) {
		JceScriptParam *pm = &sc->params[i];
		if (pm->name[0] == '\0') continue;
		if (strcmp(pm->name, name) != 0) continue;
		if (pm->kind != (uint32_t)JCE_SCRIPT_PARAM_TEXT) return "";
		pm->text[sizeof pm->text - 1] = '\0';   /* a hand-edited scene */
		return pm->text;
	}
	return "";
}

/* ── Authored curves (jce.curve_eval) ─────────────────────────────────────
 * The editor's Curve Editor writes multi-channel curve documents and, until
 * this binding, NOTHING anywhere read them: no loader, no evaluator, no
 * component field, no script call.  An author could draw a curve and there
 * was no way for the game to use it.
 *
 * Unity's shape is the one with no invention in it: the curve is an authored
 * FUNCTION and the script decides what it means.  A component would have had
 * to decide what the curve DRIVES, and every answer to that is a feature
 * nobody asked for.
 *
 * ONE EXCEPTION SINCE, and it is an exception because the answer was not
 * invented here: JceAudioSourceComponent.rolloff_curve drives volume over
 * distance, which is what Unity's Custom Rolloff curve drives on its own
 * AudioSource.  It reads through rt_curve_fetch below rather than parsing its
 * own copy, so there is still exactly one cache, one parse and one failure
 * log per document.  The paragraph above still governs every OTHER curve.
 */

/* Fetch a curve by path, parsing and caching on first use.  `out_owned` is
 * set when the caller must destroy the returned curve itself -- which happens
 * only past the cache bound, where the alternative would be to answer NULL
 * about a curve that is really there.  A real reading that is still the wrong
 * answer is the expensive kind of bug; an uncached re-parse is merely slow. */
JceCurve *rt_curve_fetch(JceRuntime *rt, const char *path,
                         bool *out_owned)
{
	int       i;
	uint64_t  size = 0;
	void     *bytes;
	JceCurve *c;

	*out_owned = false;
	if (!rt || !path || !path[0]) return NULL;

	for (i = 0; i < rt->curve_count; ++i)
		if (strcmp(rt->curves[i].path, path) == 0)
			return rt->curves[i].curve;  /* may be NULL: remembered failure */

	bytes = rt_read_asset_with_fallback(rt, path, &size);
	c = (bytes && size > 0)
	        ? jce_curve_parse((const char *)bytes, (size_t)size)
	        : NULL;
	if (bytes) jce_free(bytes);
	if (!c)
		LOG_WARN(LOG_TAG, "jce.curve_eval: cannot load curve '%s'", path);

	if (rt->curve_count < RT_CURVE_CACHE_MAX &&
	    strlen(path) < sizeof rt->curves[0].path) {
		CurveEntry *ce = &rt->curves[rt->curve_count++];
		snprintf(ce->path, sizeof ce->path, "%s", path);
		ce->curve = c;                 /* NULL is cached on purpose */
		return c;
	}

	/* Past the bound (or a path too long to key on): correct, uncached. */
	*out_owned = (c != NULL);
	return c;
}

void rt_curve_cache_clear(JceRuntime *rt)
{
	int i;
	if (!rt) return;
	/* A NULL entry is a remembered failure and jce_curve_destroy tolerates
	 * one, so this needs no per-entry test. */
	for (i = 0; i < rt->curve_count; ++i) {
		jce_curve_destroy(rt->curves[i].curve);
		rt->curves[i].curve = NULL;
		rt->curves[i].path[0] = '\0';
	}
	rt->curve_count = 0;
}

/* FALSE -- with *out_value untouched -- for every absence: no such asset, a
 * document that does not parse, no channel of that name, or a channel with no
 * keys.  A curve that genuinely evaluates to 0 and a curve that is not there
 * must not be one reading, which is why this is not `float curve_eval(...)`.
 * The same rule get_script_param follows, so
 *
 *     local k = jce.curve_eval("curves/recoil.json", "kick", t) or 0.0
 *
 * reads the way an author expects.
 *
 * A NULL or empty `channel` means THE FIRST CHANNEL -- a distinct request
 * from a name that is not there, so the two do not collide. */
static bool rt_script_curve_eval(void *user, const char *path,
                                 const char *channel, double t,
                                 double *out_value)
{
	JceRuntime *rt = (JceRuntime *)user;
	JceCurve   *c;
	bool        owned = false;
	int         ch;

	if (!rt || !out_value || !path || !path[0]) return false;

	c = rt_curve_fetch(rt, path, &owned);
	if (!c) return false;

	ch = (channel && channel[0])
	         ? jce_curve_channel_index(c, channel)
	         : (jce_curve_channel_count(c) > 0 ? 0 : -1);
	if (ch < 0 || jce_curve_key_count(c, ch) == 0) {
		if (owned) jce_curve_destroy(c);
		return false;
	}

	*out_value = (double)jce_curve_eval(c, ch, (float)t);
	if (owned) jce_curve_destroy(c);
	return true;
}

/* Build the JceScriptHost callback table from this module's bindings and
 * create the runtime's script VM (+ editor hot-reload watcher).  Moved
 * verbatim from jce_runtime_create so every rt_script_* callback can stay
 * file-static in this module. */
void rt_script_install_vm(JceRuntime *rt)
{
	JceScriptHost host = {0};
	host.user           = rt;
	host.log            = rt_script_log;
	host.get_script_param = rt_script_get_script_param;
	host.get_script_param_text = rt_script_get_script_param_text;
	host.curve_eval     = rt_script_curve_eval;
	host.is_key_down    = rt_script_is_key_down;
	host.get_position   = rt_script_get_position;
	host.set_position   = rt_script_set_position;
	host.get_rotation   = rt_script_get_rotation;
	host.set_rotation   = rt_script_set_rotation;
	host.get_scale      = rt_script_get_scale;
	host.get_world_position = rt_script_get_world_position;
	host.line_set_points    = rt_script_line_set_points;
	host.set_scale      = rt_script_set_scale;
	host.set_parent     = rt_script_set_parent;
	host.get_parent     = rt_script_get_parent;
	host.find_with_tag  = rt_script_find_with_tag;
	host.vcam_activate  = rt_script_vcam_activate;
	host.destroy_entity = rt_script_destroy_entity;
	host.spawn          = rt_script_spawn;
	host.move_axis      = rt_script_move_axis;
	host.input_button   = rt_script_input_button;
	host.set_time_scale = rt_script_set_time_scale;
	host.set_paused     = rt_script_set_paused;
	host.shake_camera   = rt_script_shake_camera;
	host.request_scene    = rt_script_request_scene;
	host.is_transitioning = rt_script_is_transitioning;
	host.audio_play       = rt_script_audio_play;
	host.audio_stop       = rt_script_audio_stop;
	host.audio_is_playing = rt_script_audio_is_playing;
	host.save_game        = rt_script_save_game;
	host.load_game        = rt_script_load_game;
	host.overlap_sphere   = rt_script_overlap_sphere;
	host.overlap_box      = rt_script_overlap_box;
	host.music_set_intensity      = rt_script_music_set_intensity;
	host.music_get_intensity      = rt_script_music_get_intensity;
	host.music_request_transition = rt_script_music_request_transition;
	host.gas_activate   = rt_script_gas_activate;
	host.gas_get        = rt_script_gas_get;
	host.gas_apply      = rt_script_gas_apply;
	host.read_file      = rt_script_read_file;
	host.raycast        = rt_script_raycast;
	host.raycast_filtered = rt_script_raycast_filtered;
	host.raycast_all      = rt_script_raycast_all;
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
	host.pointer_delta = rt_script_pointer_delta;
	host.pointer_wheel = rt_script_pointer_wheel;
	host.pointer_button = rt_script_pointer_button;
	host.touch_count = rt_script_touch_count;
	host.touch_get = rt_script_touch_get;
	host.loc_translate  = rt_script_loc_translate;
	host.loc_get_locale = rt_script_loc_get_locale;
	host.loc_set_locale = rt_script_loc_set_locale;
	host.play_sound     = rt_script_play_sound;
	host.play_sound_spatial = rt_script_play_sound_spatial;
	host.ui_get_slider  = rt_script_ui_get_slider;
	host.ui_set_slider  = rt_script_ui_set_slider;
	host.ui_get_toggle  = rt_script_ui_get_toggle;
	host.ui_set_toggle  = rt_script_ui_set_toggle;
	host.ui_get_progress = rt_script_ui_get_progress;
	host.ui_set_progress = rt_script_ui_set_progress;
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
	host.particle_set_color = rt_script_particle_set_color;
	host.find_by_name     = rt_script_find_by_name;
	host.find_by_prefix   = rt_script_find_by_prefix;
	host.comp_get_json    = rt_script_comp_get_json;
	host.comp_set_json    = rt_script_comp_set_json;
	host.render_get_json  = rt_script_render_get_json;
	host.render_set_json  = rt_script_render_set_json;
	host.json_free        = rt_script_json_free;
	host.ui_get_dropdown   = rt_script_ui_get_dropdown;
	host.ui_set_dropdown   = rt_script_ui_set_dropdown;
	host.ui_get_input_text = rt_script_ui_get_input_text;
	host.ui_set_input_text = rt_script_ui_set_input_text;
	host.ui_get_scroll     = rt_script_ui_get_scroll;
	host.ui_set_scroll     = rt_script_ui_set_scroll;
	host.audio_set_volume = rt_script_audio_set_volume;
	host.world_get_hour              = rt_script_world_get_hour;
	host.world_set_hour              = rt_script_world_set_hour;
	host.world_is_daytime            = rt_script_world_is_daytime;
	host.world_get_weather           = rt_script_world_get_weather;
	host.world_get_weather_intensity = rt_script_world_get_weather_intensity;
	host.world_get_wind_speed        = rt_script_world_get_wind_speed;

	/* The host table is built once and reused for every language's VM: all
	 * languages must see the SAME host or a cross-language differential is
	 * comparing two engines.  No VM is created here — the first script of a
	 * given language creates that language's VM (rt_script_vm_get). */
	rt->script_host       = host;
	rt->script_lang_count = 0;
	rt->script_enabled    = true;

	/* The one thing worth failing at STARTUP rather than at the first script:
	 * JCE_SCRIPT_LANGUAGE naming a language this executable cannot run.  That
	 * is a whole-run mistake — every script in the process was redirected to a
	 * VM that does not exist — so it is reported once, here, instead of once
	 * per entity, and scripting is disabled for the run rather than silently
	 * degrading to Lua. */
	{
		const char *lang = NULL;
		if (rt_script_override(&lang) && !jce_script_vm_find(lang)) {
			LOG_ERROR(LOG_TAG,
			          "JCE_SCRIPT_LANGUAGE=%s: no such script VM — scripts "
			          "are DISABLED for this run. This is not a fallback to "
			          "Lua on purpose: a run that asked for %s must not "
			          "silently be a Lua run.",
			          lang, lang);
			rt_script_log_registry_evidence();
			rt->script_enabled = false;
		} else if (lang) {
			LOG_INFO(LOG_TAG,
			         "JCE_SCRIPT_LANGUAGE=%s: OVERRIDE — every script in this "
			         "process runs in '%s' whatever its extension", lang, lang);
		}
	}

	/* Script hot-reload watcher (editor dev; inert in shipped).  The
	 * physics contact -> script on_collision bridge is wired per scene in
	 * rt_spawn_scene_state (it depends on the per-scene physics world). */
	if (rt->script_enabled)
		rt->script_watcher = jce_file_watcher_create();
}
