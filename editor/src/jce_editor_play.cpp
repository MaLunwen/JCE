/*
 * jce_editor_play.cpp  Play mode simulation and entity clipboard.
 *
 * Manages play/pause/stop state transitions, physics world lifetime,
 * audio playback, ECS system ticking, snapshot save/restore around
 * play sessions, and copy/paste of entities.
 *
 * Dogfooding: Play Mode should simulate as close to the runtime as
 * possible.  All subsystems that the published game would run are
 * enabled here so the editor is the engine's first client.
 */

#include "jce_editor_state_internal.h"
#include "jce_editor_ecs_adapter.h"
#include "scene/jce_editor_scene_render.h"

extern "C" {
#include <jce/physics/jce_physics.h>
#include <jce/audio/jce_audio.h>
#include <jce/scene/jce_scene.h>
}

#include <SDL3/SDL_iostream.h>

/* ── Play mode static data ───────────────────────────────────────── */

/* Scene snapshot taken when play starts; restored when stopped. */
static EditorHistorySnapshot s_play_snapshot;
static bool s_play_snapshot_valid = false;

/* Physics world created on play, destroyed on stop. */
static JcePhysicsWorld *s_play_physics = NULL;

/* Maps entity index -> physics body handle for rigidbody entities. */
#define PLAY_MAX_BODIES 256
static struct {
	int          entity_index;
	JceBodyHandle body;
} s_play_bodies[PLAY_MAX_BODIES];
static int s_play_body_count = 0;

/* Audio engine created on play, destroyed on stop. */
static JceAudio *s_play_audio = NULL;

#define PLAY_MAX_VOICES 64
static struct {
	uint32_t entity_id;
	JceSound sound;
	JceVoice voice;
} s_play_voices[PLAY_MAX_VOICES];
static int s_play_voice_count = 0;

/* ── Physics helpers ─────────────────────────────────────────────── */

static void play_create_physics_world(void)
{
	JcePhysicsWorldDesc desc;
	memset(&desc, 0, sizeof(desc));
	desc.gravity.x = 0.0f;
	desc.gravity.y = -9.81f;
	desc.gravity.z = 0.0f;
	desc.fixed_timestep = 1.0f / 60.0f;
	s_play_physics = jce_physics_create(&desc);
	s_play_body_count = 0;

	if (!s_play_physics) {
		LOG_ERROR(LOG_TAG, "failed to create physics world for play mode");
		return;
	}

	/* Create physics bodies for entities with rigidbody + transform. */
	for (int i = 0; i < s.entity_count && s_play_body_count < PLAY_MAX_BODIES; i++) {
		JceEntityInfo *e = &s.entities[i];
		if (e->id == 0 || !e->enabled) continue;

		const JceComponentInfo *rb_comp = NULL;
		const JceComponentInfo *tf_comp = NULL;
		int comp_count = 0;
		JceComponentInfo *comps = jce_state_get_entity_components(e->id, &comp_count);
		for (int c = 0; c < comp_count; c++) {
			if (comps[c].type == JCE_COMP_RIGIDBODY) rb_comp = &comps[c];
			if (comps[c].type == JCE_COMP_TRANSFORM) tf_comp = &comps[c];
		}
		if (!rb_comp || !tf_comp) continue;

		JceBodyDesc bd;
		memset(&bd, 0, sizeof(bd));
		bd.position.x = tf_comp->data.transform.pos[0];
		bd.position.y = tf_comp->data.transform.pos[1];
		bd.position.z = tf_comp->data.transform.pos[2];
		bd.rotation   = jce_q_identity();
		bd.mass       = rb_comp->data.rigidbody.mass;
		bd.linear_damping  = rb_comp->data.rigidbody.drag;
		bd.angular_damping = rb_comp->data.rigidbody.angular_drag;
		bd.friction    = 0.5f;
		bd.restitution = 0.0f;
		bd.shape       = JCE_SHAPE_SPHERE;
		bd.half_extents.x = 0.5f;
		bd.type        = rb_comp->data.rigidbody.is_kinematic
		                 ? JCE_BODY_KINEMATIC : JCE_BODY_DYNAMIC;

		JceBodyHandle body = jce_physics_body_create(s_play_physics, &bd);
		if (jce_body_valid(body)) {
			s_play_bodies[s_play_body_count].entity_index = i;
			s_play_bodies[s_play_body_count].body = body;
			s_play_body_count++;
		}
	}

	LOG_INFO(LOG_TAG, "play physics: %d bodies created", s_play_body_count);
}

static void play_destroy_physics_world(void)
{
	if (s_play_physics) {
		jce_physics_destroy(s_play_physics);
		s_play_physics = NULL;
	}
	s_play_body_count = 0;
}

static void play_sync_physics_to_entities(void)
{
	if (!s_play_physics) return;

	for (int i = 0; i < s_play_body_count; i++) {
		int eidx = s_play_bodies[i].entity_index;
		if (eidx < 0 || eidx >= s.entity_count) continue;

		jce_vec3 pos;
		jce_quat rot;
		jce_physics_body_get_transform(s_play_physics,
		                               s_play_bodies[i].body, &pos, &rot);

		/* Update the editor cache (for viewport rendering). */
		JceComponentInfo *comps = s.components[eidx];
		for (int c = 0; c < JCE_MAX_COMPONENTS; c++) {
			if (comps[c].type == JCE_COMP_TRANSFORM) {
				comps[c].data.transform.pos[0] = pos.x;
				comps[c].data.transform.pos[1] = pos.y;
				comps[c].data.transform.pos[2] = pos.z;
				break;
			}
		}

		/* Push the updated transform to ECS (keeps authoritative store
		 * in sync so any system reading from ECS sees current positions). */
		JceEntity ecs_e = (JceEntity)s.entities[eidx].ecs_entity;
		if (s.scene && ecs_e != 0) {
			JceTransform *tc = jce_scene_get_transform(s.scene, ecs_e);
			if (tc) {
				tc->position = pos;
				tc->rotation = rot;
			}
		}
	}
}

/* ── Audio helpers ────────────────────────────────────────────────── */

/* Resolve a clip_path (relative to scene dir) and load into the
 * play-mode audio engine.  Returns JCE_SOUND_INVALID on failure. */
static JceSound play_load_audio_clip(const char *clip_path)
{
	if (!s_play_audio || !clip_path || clip_path[0] == '\0')
		return JCE_SOUND_INVALID;

	/* Build absolute path from scene dir + clip_path. */
	const char *scene_path = jce_state_get_current_scene_path();
	char full[1024];
	if (scene_path && scene_path[0] != '\0') {
		/* Derive directory from scene file path. */
		char dir[512];
		snprintf(dir, sizeof(dir), "%s", scene_path);
		char *sep = strrchr(dir, '/');
		char *bsep = strrchr(dir, '\\');
		if (bsep && (!sep || bsep > sep)) sep = bsep;
		if (sep) *sep = '\0';
		else dir[0] = '\0';
		snprintf(full, sizeof(full), "%s/%s", dir, clip_path);
	} else {
		snprintf(full, sizeof(full), "%s", clip_path);
	}

	/* Read file into memory. */
	size_t fsize = 0;
	void *data = SDL_LoadFile(full, &fsize);
	if (!data || fsize == 0) {
		LOG_WARN(LOG_TAG, "play audio: could not read '%s'", full);
		return JCE_SOUND_INVALID;
	}

	JceSound snd = jce_audio_load_memory(s_play_audio, data, (uint32_t)fsize, clip_path);
	SDL_free(data);
	return snd;
}

static void play_start_audio(void)
{
	s_play_audio = jce_audio_create();
	s_play_voice_count = 0;

	if (!s_play_audio) {
		LOG_WARN(LOG_TAG, "failed to create audio engine for play mode");
		return;
	}

	/* Iterate entities looking for AudioSource components. */
	for (int i = 0; i < s.entity_count && s_play_voice_count < PLAY_MAX_VOICES; i++) {
		JceEntityInfo *e = &s.entities[i];
		if (e->id == 0 || !e->enabled) continue;

		int comp_count = 0;
		JceComponentInfo *comps = jce_state_get_entity_components(e->id, &comp_count);
		for (int c = 0; c < comp_count; c++) {
			if (comps[c].type != JCE_COMP_AUDIO_SOURCE) continue;
			if (!comps[c].data.audio_source.play_on_awake) continue;

			JceSound snd = play_load_audio_clip(comps[c].data.audio_source.clip_path);
			if (snd == JCE_SOUND_INVALID) continue;

			JceVoice v = jce_audio_play(s_play_audio, snd,
				comps[c].data.audio_source.loop,
				comps[c].data.audio_source.volume,
				comps[c].data.audio_source.pitch);

			s_play_voices[s_play_voice_count].entity_id = e->id;
			s_play_voices[s_play_voice_count].sound     = snd;
			s_play_voices[s_play_voice_count].voice     = v;
			s_play_voice_count++;
			break;
		}
	}

	LOG_INFO(LOG_TAG, "play audio: %d voices started", s_play_voice_count);
}

static void play_stop_audio(void)
{
	if (s_play_audio) {
		jce_audio_stop_all(s_play_audio);
		for (int i = 0; i < s_play_voice_count; i++)
			jce_audio_unload(s_play_audio, s_play_voices[i].sound);
		jce_audio_destroy(s_play_audio);
		s_play_audio = NULL;
	}
	s_play_voice_count = 0;
}

static void play_pause_audio(void)
{
	if (!s_play_audio) return;
	for (int i = 0; i < s_play_voice_count; i++)
		jce_audio_pause(s_play_audio, s_play_voices[i].voice);
}

static void play_resume_audio(void)
{
	if (!s_play_audio) return;
	for (int i = 0; i < s_play_voice_count; i++)
		jce_audio_resume(s_play_audio, s_play_voices[i].voice);
}

/* ── Play mode API ───────────────────────────────────────────────── */

void jce_state_play(void)
{
	if (s.play_state == JCE_PLAY_STOPPED) {
		/* Capture scene snapshot before entering play mode. */
		s_play_snapshot_valid = history_capture_snapshot(&s_play_snapshot);
		if (!s_play_snapshot_valid)
			LOG_WARN(LOG_TAG, "failed to capture play-mode snapshot");

		play_create_physics_world();
		play_start_audio();
		jce_editor_scene_reset_anim_timer();
		s.play_state = JCE_PLAY_PLAYING;
		LOG_INFO(LOG_TAG, "play mode started");
	}
}

void jce_state_pause(void)
{
	if (s.play_state == JCE_PLAY_PLAYING) {
		play_pause_audio();
		s.play_state = JCE_PLAY_PAUSED;
		LOG_INFO(LOG_TAG, "play mode paused");
	} else if (s.play_state == JCE_PLAY_PAUSED) {
		play_resume_audio();
		s.play_state = JCE_PLAY_PLAYING;
		LOG_INFO(LOG_TAG, "play mode resumed");
	}
}

void jce_state_stop(void)
{
	if (s.play_state != JCE_PLAY_STOPPED) {
		play_destroy_physics_world();
		play_stop_audio();

		/* Restore scene to pre-play state. */
		if (s_play_snapshot_valid) {
			history_restore_snapshot(s_play_snapshot, "play-stop-restore");
			s_play_snapshot = EditorHistorySnapshot();
			s_play_snapshot_valid = false;
		}

		s.play_state = JCE_PLAY_STOPPED;
		jce_editor_scene_reset_anim_timer();
		LOG_INFO(LOG_TAG, "play mode stopped");
	}
}

JcePlayState jce_state_get_play_state(void) { return s.play_state; }

void jce_state_play_mode_tick(float dt)
{
	if (s.play_state != JCE_PLAY_PLAYING) return;

	/* Step physics simulation. */
	if (s_play_physics) {
		jce_physics_step(s_play_physics, dt);
		play_sync_physics_to_entities();
	}

	/* Progress ECS systems (flecs registered systems, if any).
	 * This is the same call the runtime uses — dogfooding. */
	if (s.scene)
		jce_scene_update(s.scene, dt);
}

/* ── Entity Clipboard ────────────────────────────────────────────── */

static struct {
	uint32_t    id;
	char        name[JCE_MAX_ENTITY_NAME];
	JceTagColor tag_color;
	char        tag[JCE_MAX_TAG_STRING];
	bool        prefab_instance;
	char        prefab_path[JCE_MAX_PREFAB_PATH];
} s_clipboard;

void jce_state_copy_entity(uint32_t id)
{
	JceEntityInfo *e = jce_state_get_entity(id);
	if (!e) return;
	s_clipboard.id = id;
	snprintf(s_clipboard.name, sizeof(s_clipboard.name), "%s", e->name);
	s_clipboard.tag_color = e->tag_color;
	snprintf(s_clipboard.tag, sizeof(s_clipboard.tag), "%s", e->tag);
	s_clipboard.prefab_instance = e->prefab_instance;
	snprintf(s_clipboard.prefab_path, sizeof(s_clipboard.prefab_path), "%s", e->prefab_path);
	LOG_INFO(LOG_TAG, "copied entity %u (%s)", id, e->name);
}

uint32_t jce_state_paste_entity(uint32_t parent_id)
{
	if (s_clipboard.id == 0) return 0;

	char paste_name[JCE_MAX_ENTITY_NAME];
	snprintf(paste_name, sizeof(paste_name), "%s (Paste)", s_clipboard.name);
	uint32_t new_id = jce_state_create_entity(paste_name, parent_id);

	JceEntityInfo *e = jce_state_get_entity(new_id);
	if (e) {
		e->tag_color = s_clipboard.tag_color;
		snprintf(e->tag, sizeof(e->tag), "%s", s_clipboard.tag);
		e->prefab_instance = s_clipboard.prefab_instance;
		snprintf(e->prefab_path, sizeof(e->prefab_path), "%s", s_clipboard.prefab_path);
	}

	LOG_INFO(LOG_TAG, "pasted entity as %u (%s)", new_id, paste_name);
	return new_id;
}

bool jce_state_has_copied(void)
{
	return s_clipboard.id != 0;
}
