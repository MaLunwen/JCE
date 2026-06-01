/*
 * jce_runtime.c  Layer 6 — Application.
 *
 * Authored-scene → live simulation driver shared by the editor's Play
 * button and deployed game executables.  See jce_runtime.h for contract.
 *
 * Walks the JceScene once at create() time, materialising:
 *   - one JcePhysicsWorld with a body for every (RigidBody +
 *     Box/SphereCollider) pair and a JceCharacterHandle for the first
 *     CharacterController found,
 *   - a JceVoice per AudioSource flagged play_on_awake.
 *
 * Per-step:
 *   1. character controller move + jump from the latest input
 *   2. jce_physics_step
 *   3. write back transforms
 *   4. jce_scene_update
 *   5. update audio listener (tracks primary camera) + spatial voices
 *
 * Implementation is intentionally allocation-heavy on create() rather
 * than per-frame: bodies/voices are sized to the actual scene contents,
 * not a fixed cap.
 */

#include <jce/application/jce_runtime.h>

#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/physics/jce_physics_types.h>
#include <jce/middleware/physics/jce_collider_cook.h>
#include <jce/middleware/physics/jce_collider_asset.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/resource/jce_model_importer.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LOG_TAG "runtime"

/* ── Internal types ──────────────────────────────────────────────── */

typedef struct {
	JceEntity     entity;
	JceBodyHandle body;
} BodyEntry;

typedef struct {
	JceEntity entity;
	JceSound  sound;
	JceVoice  voice;
	bool      spatial;
} VoiceEntry;

struct JceRuntime {
	JceScene        *scene;       /* not owned */
	JcePakArchive   *pak;         /* not owned */
	JceAudio        *audio;       /* not owned */

	uint32_t      (*audio_load_fn)(void *, JceAudio *, const char *);
	void            *user_data;

	JcePhysicsWorld *physics;     /* owned (NULL if !enable_physics) */

	BodyEntry       *bodies;
	int              body_count;
	int              body_cap;

	JceCharacterHandle character;
	JceEntity          character_entity;

	VoiceEntry      *voices;
	int              voice_count;
	int              voice_cap;

	JceRuntimeInput  input;
};

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Sum local positions up the parent chain to get a world-space
 * position.  Rotation/scale ignored — adequate for audio distance
 * attenuation and listener placement.  Matches the editor Play impl. */
static jce_vec3 rt_world_position(JceScene *scene, JceEntity e)
{
	jce_vec3 acc = { 0.0f, 0.0f, 0.0f };
	while (e != 0 && jce_scene_has_transform(scene, e)) {
		JceTransform *t = jce_scene_get_transform(scene, e);
		if (t) {
			acc.x += t->position.x;
			acc.y += t->position.y;
			acc.z += t->position.z;
		}
		e = jce_scene_get_parent(scene, e);
	}
	return acc;
}

static bool rt_grow_bodies(JceRuntime *rt)
{
	int new_cap = rt->body_cap ? rt->body_cap * 2 : 16;
	BodyEntry *p = (BodyEntry *)jce_realloc(rt->bodies,
	                                         (size_t)new_cap * sizeof(*p));
	if (!p) return false;
	rt->bodies   = p;
	rt->body_cap = new_cap;
	return true;
}

static bool rt_grow_voices(JceRuntime *rt)
{
	int new_cap = rt->voice_cap ? rt->voice_cap * 2 : 8;
	VoiceEntry *p = (VoiceEntry *)jce_realloc(rt->voices,
	                                           (size_t)new_cap * sizeof(*p));
	if (!p) return false;
	rt->voices   = p;
	rt->voice_cap = new_cap;
	return true;
}

/* ── Scene walk: spawn physics + audio for each entity ───────────── */

/*
 * Try to materialise a per-object compound collider for entity `e`.
 * Loads the referenced model WITHOUT flattening its node hierarchy, cooks
 * each part into its own child shape, and instantiates the lot as a single
 * compound body — so a model holding N separated objects yields N child
 * colliders rather than one fat hull spanning the gaps between them.
 *
 * Returns true if a body was spawned (caller then skips the regular
 * rigid-body path so the entity does not get a second body).
 */
static bool rt_try_spawn_compound(JceRuntime *rt, JceScene *scene,
                                  JceEntity e, const JceTransform *tf)
{
	JceCompoundColliderComponent *cc = jce_scene_get_compound_collider(scene, e);
	if (!cc || cc->model_path[0] == '\0') return false;

	/* Load parts from the pak (deployed) or the host filesystem (editor). */
	JceModelParts parts;
	memset(&parts, 0, sizeof parts);
	bool loaded = false;
	if (rt->pak) {
		const JcePakAsset *asset = jce_pak_find(rt->pak, cc->model_path);
		if (asset) {
			void *buf = jce_malloc((size_t)asset->original_size);
			if (buf) {
				size_t n = jce_pak_decompress(asset, buf,
				                              (size_t)asset->original_size);
				if (n > 0) {
					const char *ext = strrchr(cc->model_path, '.');
					loaded = jce_model_importer_load_parts_memory(
						buf, n, ext ? ext : "", &parts);
				}
				jce_free(buf);
			}
		}
	}
	if (!loaded)
		loaded = jce_model_importer_load_parts_file(cc->model_path, &parts);
	if (!loaded) {
		LOG_WARN(LOG_TAG, "compound collider: cannot load %s", cc->model_path);
		return false;
	}

	JceColliderPart *cparts =
		(JceColliderPart *)jce_malloc((size_t)parts.count * sizeof(*cparts));
	if (!cparts) { jce_model_importer_free_parts(&parts); return false; }
	for (uint32_t i = 0; i < parts.count; i++) {
		cparts[i].name         = parts.parts[i].name;
		cparts[i].vertices     = parts.parts[i].positions;
		cparts[i].vertex_count = parts.parts[i].vertex_count;
		cparts[i].indices      = parts.parts[i].indices;
		cparts[i].index_count  = parts.parts[i].index_count;
		memcpy(cparts[i].transform, parts.parts[i].transform,
		       sizeof cparts[i].transform);
	}

	JceColliderCookConfig cfg = jce_collider_cook_config_default();
	cfg.mode          = (JceColliderMode)cc->mode;
	cfg.split         = (JceColliderSplitMode)cc->split;
	cfg.is_static     = cc->is_static;
	cfg.detect_naming = cc->detect_naming;
	if (cc->vhacd_resolution)         cfg.vhacd_resolution = cc->vhacd_resolution;
	if (cc->vhacd_max_hulls)          cfg.vhacd_max_hulls = cc->vhacd_max_hulls;
	if (cc->vhacd_max_verts_per_hull) cfg.vhacd_max_verts_per_hull = cc->vhacd_max_verts_per_hull;

	JceCookedCollider cooked;
	bool cooked_ok = jce_collider_cook(cparts, parts.count, &cfg, &cooked);
	jce_free(cparts);
	jce_model_importer_free_parts(&parts);
	if (!cooked_ok) {
		LOG_WARN(LOG_TAG, "compound collider: cook failed for %s", cc->model_path);
		return false;
	}

	JceColliderInstanceDesc id;
	memset(&id, 0, sizeof id);
	id.position    = tf->position;
	id.rotation    = jce_q_identity();
	id.friction    = cc->friction > 0.0f ? cc->friction : 0.5f;
	id.restitution = cc->restitution;
	id.is_trigger  = cc->is_trigger;

	JceRigidBodyComponent *rb = jce_scene_get_rigidbody(scene, e);
	if (rb) {
		id.mass            = rb->mass;
		id.linear_damping  = rb->drag;
		id.angular_damping = rb->angular_drag;
		if (rb->is_kinematic)      id.type = JCE_BODY_KINEMATIC;
		else if (rb->mass <= 0.0f) id.type = JCE_BODY_STATIC;
		else                       id.type = JCE_BODY_DYNAMIC;
	} else {
		id.type = JCE_BODY_STATIC;
	}

	JceBodyHandle body = jce_collider_instantiate(rt->physics, &cooked, &id);
	jce_collider_cooked_free(&cooked);
	if (!jce_body_valid(body)) return false;

	if (rt->body_count >= rt->body_cap && !rt_grow_bodies(rt))
		return true;   /* spawned but cannot track — still skip box path */
	rt->bodies[rt->body_count].entity = e;
	rt->bodies[rt->body_count].body   = body;
	rt->body_count++;
	return true;
}

static void rt_spawn_entity(JceScene *scene, JceEntity e, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (!jce_scene_has_transform(scene, e)) return;
	JceTransform *tf = jce_scene_get_transform(scene, e);
	if (!tf) return;

	/* ── Character controller (only one supported per scene) ── */
	if (rt->physics && !jce_character_valid(rt->character)) {
		JceCharacterControllerComponent *cc =
			jce_scene_get_character_controller(scene, e);
		if (cc) {
			JceCharacterDesc cd;
			memset(&cd, 0, sizeof cd);
			cd.position      = tf->position;
			cd.radius        = cc->radius      > 0.0f ? cc->radius      : 0.35f;
			cd.height        = cc->height      > 0.0f ? cc->height      : 1.8f;
			cd.step_height   = cc->step_offset > 0.0f ? cc->step_offset : 0.35f;
			cd.max_slope_deg = cc->slope_limit > 0.0f ? cc->slope_limit : 50.0f;
			cd.gravity       = 9.81f;
			cd.jump_speed    = 5.0f;
			rt->character = jce_physics_character_create(rt->physics, &cd);
			if (jce_character_valid(rt->character))
				rt->character_entity = e;
			/* Character takes priority — skip rigid body for this entity. */
			goto try_audio;
		}
	}

	/* ── Compound collider (per-object cooked) takes priority ── */
	if (rt->physics && rt_try_spawn_compound(rt, scene, e, tf))
		goto try_audio;

	/* ── Rigid body ── */
	if (rt->physics) {
		JceRigidBodyComponent *rb = jce_scene_get_rigidbody(scene, e);
		if (rb) {
			JceBodyDesc bd;
			memset(&bd, 0, sizeof bd);
			bd.position        = tf->position;
			bd.rotation        = jce_q_identity();
			bd.mass            = rb->mass;
			bd.linear_damping  = rb->drag;
			bd.angular_damping = rb->angular_drag;
			bd.friction        = rb->friction    > 0.0f ? rb->friction    : 0.5f;
			bd.restitution     = rb->restitution;

			JceBoxColliderComponent    *box = jce_scene_get_box_collider(scene, e);
			JceSphereColliderComponent *sph = jce_scene_get_sphere_collider(scene, e);
			if (box) {
				bd.shape = JCE_SHAPE_BOX;
				bd.half_extents.x = 0.5f * box->size[0] * tf->scale.x;
				bd.half_extents.y = 0.5f * box->size[1] * tf->scale.y;
				bd.half_extents.z = 0.5f * box->size[2] * tf->scale.z;
				if (bd.half_extents.x <= 0.0f) bd.half_extents.x = 0.5f;
				if (bd.half_extents.y <= 0.0f) bd.half_extents.y = 0.5f;
				if (bd.half_extents.z <= 0.0f) bd.half_extents.z = 0.5f;
				bd.position.x += box->center[0];
				bd.position.y += box->center[1];
				bd.position.z += box->center[2];
				bd.is_trigger = box->is_trigger;
			} else if (sph) {
				bd.shape = JCE_SHAPE_SPHERE;
				float smax = tf->scale.x;
				if (tf->scale.y > smax) smax = tf->scale.y;
				if (tf->scale.z > smax) smax = tf->scale.z;
				float r = sph->radius > 0.0f ? sph->radius : 0.5f;
				bd.half_extents.x = r * smax;
				bd.position.x += sph->center[0];
				bd.position.y += sph->center[1];
				bd.position.z += sph->center[2];
				bd.is_trigger = sph->is_trigger;
			} else {
				/* No collider authored — placeholder box from transform
				 * scale so dropped objects still collide. */
				bd.shape = JCE_SHAPE_BOX;
				bd.half_extents.x = 0.5f * tf->scale.x;
				bd.half_extents.y = 0.5f * tf->scale.y;
				bd.half_extents.z = 0.5f * tf->scale.z;
				if (bd.half_extents.x <= 0.0f) bd.half_extents.x = 0.5f;
				if (bd.half_extents.y <= 0.0f) bd.half_extents.y = 0.5f;
				if (bd.half_extents.z <= 0.0f) bd.half_extents.z = 0.5f;
			}

			if (rb->is_kinematic)       bd.type = JCE_BODY_KINEMATIC;
			else if (rb->mass <= 0.0f)  bd.type = JCE_BODY_STATIC;
			else                        bd.type = JCE_BODY_DYNAMIC;

			JceBodyHandle body = jce_physics_body_create(rt->physics, &bd);
			if (jce_body_valid(body)) {
				if (rb->ccd_mode != JCE_CCD_DISCRETE) {
					jce_physics_body_set_ccd_mode(rt->physics, body,
					                              (JceCcdMode)rb->ccd_mode);
					if (rb->ccd_threshold > 0.0f)
						jce_physics_body_set_ccd_motion_threshold(
							rt->physics, body, rb->ccd_threshold);
					if (rb->ccd_sphere_radius > 0.0f)
						jce_physics_body_set_ccd_swept_sphere_radius(
							rt->physics, body, rb->ccd_sphere_radius);
				}
				if (rt->body_count >= rt->body_cap && !rt_grow_bodies(rt))
					goto try_audio;
				rt->bodies[rt->body_count].entity = e;
				rt->bodies[rt->body_count].body   = body;
				rt->body_count++;
			}
		}
	}

try_audio:
	/* ── Audio source ── */
	if (rt->audio && (rt->pak || rt->audio_load_fn)) {
		JceAudioSourceComponent *as = jce_scene_get_audio_source(scene, e);
		if (as && as->play_on_awake && as->clip_path[0]) {
			JceSound snd = rt->audio_load_fn
			               ? rt->audio_load_fn(rt->user_data, rt->audio, as->clip_path)
			               : jce_audio_load(rt->audio, rt->pak, as->clip_path);
			if (snd != JCE_SOUND_INVALID) {
				float vol   = as->volume > 0.0f ? as->volume : 1.0f;
				float pitch = as->pitch  > 0.0f ? as->pitch  : 1.0f;
				JceVoice v = jce_audio_play(rt->audio, snd, as->loop, vol, pitch);
				bool spatial = (as->spatial_blend > 0.5f);
				if (spatial) {
					jce_audio_voice_set_3d(rt->audio, v, true);
					jce_vec3 wp = rt_world_position(scene, e);
					jce_audio_voice_set_position(rt->audio, v, wp.x, wp.y, wp.z);
					jce_audio_voice_set_attenuation(rt->audio, v,
					                                JCE_AUDIO_ATTEN_INVERSE,
					                                1.0f, 25.0f, 1.0f);
				} else {
					jce_audio_voice_set_3d(rt->audio, v, false);
				}
				if (rt->voice_count >= rt->voice_cap && !rt_grow_voices(rt))
					return;
				rt->voices[rt->voice_count].entity  = e;
				rt->voices[rt->voice_count].sound   = snd;
				rt->voices[rt->voice_count].voice   = v;
				rt->voices[rt->voice_count].spatial = spatial;
				rt->voice_count++;
			} else {
				jce_log_write(JCE_LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__,
				              "audio_source: failed to load '%s'", as->clip_path);
			}
		}
	}
}

/* ── Per-frame ────────────────────────────────────────────────────── */

static void rt_drive_character(JceRuntime *rt, float dt)
{
	if (!rt->physics || !jce_character_valid(rt->character)) return;
	if (dt <= 0.0f) dt = 1.0f / 60.0f;

	float    m  = rt->input.speed_mult > 0.0f ? rt->input.speed_mult : 1.0f;
	jce_vec3 walk = jce_v3(rt->input.walk_x * m, 0.0f, rt->input.walk_z * m);
	jce_physics_character_move(rt->physics, rt->character, walk, dt);

	if (rt->input.jump_pressed) {
		if (jce_physics_character_is_grounded(rt->physics, rt->character))
			jce_physics_character_jump(rt->physics, rt->character);
		rt->input.jump_pressed = false;
	}
}

static void rt_sync_transforms(JceRuntime *rt)
{
	if (!rt->physics || !rt->scene) return;

	for (int i = 0; i < rt->body_count; ++i) {
		jce_vec3 p; jce_quat q;
		jce_physics_body_get_transform(rt->physics, rt->bodies[i].body, &p, &q);
		JceTransform *tc = jce_scene_get_transform(rt->scene,
		                                            rt->bodies[i].entity);
		if (tc) { tc->position = p; tc->rotation = q; }
	}

	if (jce_character_valid(rt->character) && rt->character_entity != 0) {
		jce_vec3 cp;
		jce_physics_character_get_position(rt->physics, rt->character, &cp);
		JceTransform *tc = jce_scene_get_transform(rt->scene,
		                                            rt->character_entity);
		if (tc) tc->position = cp;
	}
}

typedef struct {
	JceScene *scene;
	jce_vec3  pos;
	bool      found;
} CamScanCtx;

static void rt_pick_primary_cam(JceScene *s, JceEntity e, void *ud)
{
	CamScanCtx *ctx = (CamScanCtx *)ud;
	if (ctx->found) return;
	JceCameraComponent *cam = jce_scene_get_camera(s, e);
	if (!cam || !cam->is_primary) return;
	ctx->pos   = rt_world_position(s, e);
	ctx->found = true;
}

/* Attach the listener to the primary camera's world position; push live
 * world positions to every spatial voice so distance attenuation tracks
 * scene movement. */
static void rt_update_audio_3d(JceRuntime *rt)
{
	if (!rt->audio || !rt->scene) return;

	JceAudioListener L;
	memset(&L, 0, sizeof L);
	L.forward[2] = -1.0f;
	L.up[1]      =  1.0f;

	CamScanCtx ctx = { rt->scene, { 0.0f, 0.0f, 0.0f }, false };
	jce_scene_each_entity(rt->scene, rt_pick_primary_cam, &ctx);
	if (ctx.found) {
		L.position[0] = ctx.pos.x;
		L.position[1] = ctx.pos.y;
		L.position[2] = ctx.pos.z;
	}
	jce_audio_set_listener(rt->audio, &L);

	for (int i = 0; i < rt->voice_count; ++i) {
		if (!rt->voices[i].spatial) continue;
		jce_vec3 wp = rt_world_position(rt->scene, rt->voices[i].entity);
		jce_audio_voice_set_position(rt->audio, rt->voices[i].voice,
		                              wp.x, wp.y, wp.z);
	}
}

/* ── Public API ──────────────────────────────────────────────────── */

JCE_API JceRuntime *JCE_CALL jce_runtime_create(const JceRuntimeDesc *desc)
{
	if (!desc || !desc->scene) return NULL;

	JceRuntime *rt = (JceRuntime *)jce_malloc(sizeof *rt);
	if (!rt) return NULL;
	memset(rt, 0, sizeof *rt);

	rt->scene             = desc->scene;
	rt->pak               = desc->pak;
	rt->audio             = desc->audio;
	rt->audio_load_fn     = desc->audio_load_fn;
	rt->user_data         = desc->user_data;
	rt->character         = JCE_CHARACTER_INVALID;
	rt->character_entity  = 0;
	rt->input.speed_mult  = 1.0f;

	if (desc->enable_physics) {
		JcePhysicsWorldDesc wd;
		memset(&wd, 0, sizeof wd);
		wd.gravity.x       = 0.0f;
		wd.gravity.y       = desc->gravity_y != 0.0f ? desc->gravity_y : -9.81f;
		wd.gravity.z       = 0.0f;
		wd.fixed_timestep  = desc->fixed_timestep > 0.0f
		                     ? desc->fixed_timestep : 1.0f / 60.0f;
		rt->physics = jce_physics_create(&wd);
		if (!rt->physics) {
			jce_log_write(JCE_LOG_LEVEL_ERROR, LOG_TAG, __FILE__, __LINE__,
			              "%s", "failed to create physics world");
		}
	}

	/* One pass over the scene to instantiate bodies, character, and
	 * voices.  Components missing from the scene are silently skipped. */
	jce_scene_each_entity(rt->scene, rt_spawn_entity, rt);

	jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
	              "runtime: physics=%s bodies=%d character=%s voices=%d",
	              rt->physics ? "on" : "off",
	              rt->body_count,
	              jce_character_valid(rt->character) ? "yes" : "no",
	              rt->voice_count);
	return rt;
}

JCE_API void JCE_CALL jce_runtime_destroy(JceRuntime *rt)
{
	if (!rt) return;

	if (rt->audio) {
		for (int i = 0; i < rt->voice_count; ++i)
			jce_audio_stop(rt->audio, rt->voices[i].voice);
		for (int i = 0; i < rt->voice_count; ++i)
			jce_audio_unload(rt->audio, rt->voices[i].sound);
	}
	jce_free(rt->voices);

	if (rt->physics) {
		if (jce_character_valid(rt->character))
			jce_physics_character_destroy(rt->physics, rt->character);
		jce_physics_destroy(rt->physics);
	}
	jce_free(rt->bodies);

	jce_free(rt);
}

JCE_API void JCE_CALL jce_runtime_step(JceRuntime *rt, float dt)
{
	if (!rt) return;
	if (dt <= 0.0f) dt = 1.0f / 60.0f;

	if (rt->physics) {
		rt_drive_character(rt, dt);
		jce_physics_step(rt->physics, dt);
		rt_sync_transforms(rt);
	}
	if (rt->scene)
		jce_scene_update(rt->scene, dt);
	rt_update_audio_3d(rt);
}

JCE_API void JCE_CALL jce_runtime_set_input(JceRuntime *rt,
                                             const JceRuntimeInput *in)
{
	if (!rt || !in) return;
	rt->input.walk_x      = in->walk_x;
	rt->input.walk_z      = in->walk_z;
	if (in->jump_pressed) rt->input.jump_pressed = true;   /* sticky */
	rt->input.speed_mult  = in->speed_mult > 0.0f ? in->speed_mult : 1.0f;
}

JCE_API bool JCE_CALL jce_runtime_get_player_position(const JceRuntime *rt,
                                                       jce_vec3 *out_pos)
{
	if (!rt || !rt->physics || !jce_character_valid(rt->character)) return false;
	jce_vec3 p;
	jce_physics_character_get_position(rt->physics, rt->character, &p);
	if (out_pos) *out_pos = p;
	return true;
}

JCE_API void JCE_CALL jce_runtime_pause_audio(JceRuntime *rt)
{
	if (!rt || !rt->audio) return;
	for (int i = 0; i < rt->voice_count; ++i)
		jce_audio_pause(rt->audio, rt->voices[i].voice);
}

JCE_API void JCE_CALL jce_runtime_resume_audio(JceRuntime *rt)
{
	if (!rt || !rt->audio) return;
	for (int i = 0; i < rt->voice_count; ++i)
		jce_audio_resume(rt->audio, rt->voices[i].voice);
}

JCE_API JcePhysicsWorld *JCE_CALL jce_runtime_physics(const JceRuntime *rt)
{
	return rt ? rt->physics : NULL;
}

JCE_API JceAudio *JCE_CALL jce_runtime_audio(const JceRuntime *rt)
{
	return rt ? rt->audio : NULL;
}

JCE_API JceScene *JCE_CALL jce_runtime_scene(const JceRuntime *rt)
{
	return rt ? rt->scene : NULL;
}
