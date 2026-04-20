/*
 * jce_editor_play.cpp  Play mode simulation and entity clipboard.
 *
 * Manages play/pause/stop state transitions, physics world lifetime,
 * audio playback, ECS system ticking, snapshot save/restore around
 * play sessions, and copy/paste of entities.
 *
 * Reads everything directly from the engine ECS (JceScene) — no editor
 * mirror store.
 */

#include "jce_editor_state_internal.h"
#include "scene/jce_editor_scene_render.h"

extern "C" {
#include <jce/physics/jce_physics.h>
#include <jce/audio/jce_audio.h>
#include <jce/scene/jce_scene.h>
}

#include <SDL3/SDL_iostream.h>

/* ── Play mode static data ───────────────────────────────────────── */

static EditorHistorySnapshot s_play_snapshot;
static bool s_play_snapshot_valid = false;

static JcePhysicsWorld *s_play_physics = NULL;

#define PLAY_MAX_BODIES 256
static struct {
	int           entity_index;   /* index into g_entity_order */
	JceBodyHandle body;
} s_play_bodies[PLAY_MAX_BODIES];
static int s_play_body_count = 0;

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
	const int entity_count = (int)g_entity_order.size();
	for (int i = 0; i < entity_count && s_play_body_count < PLAY_MAX_BODIES; i++) {
		uint32_t id = g_entity_order[i];
		if (!jce_state_entity_enabled(id)) continue;

		JceEntity e = (JceEntity)id;
		JceRigidBodyComponent *rb = jce_scene_get_rigidbody(s.scene, e);
		JceTransform *tf = jce_scene_get_transform(s.scene, e);
		if (!rb || !tf) continue;

		JceBodyDesc bd;
		memset(&bd, 0, sizeof(bd));
		bd.position       = tf->position;
		bd.rotation       = jce_q_identity();
		bd.mass           = rb->mass;
		bd.linear_damping  = rb->drag;
		bd.angular_damping = rb->angular_drag;
		bd.friction    = 0.5f;
		bd.restitution = 0.0f;
		bd.shape       = JCE_SHAPE_SPHERE;
		bd.half_extents.x = 0.5f;
		bd.type        = rb->is_kinematic
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

	const int entity_count = (int)g_entity_order.size();
	for (int i = 0; i < s_play_body_count; i++) {
		int eidx = s_play_bodies[i].entity_index;
		if (eidx < 0 || eidx >= entity_count) continue;

		jce_vec3 pos;
		jce_quat rot;
		jce_physics_body_get_transform(s_play_physics,
		                               s_play_bodies[i].body, &pos, &rot);

		uint32_t id = g_entity_order[eidx];
		if (s.scene && id != 0) {
			JceTransform *tc = jce_scene_get_transform(s.scene, (JceEntity)id);
			if (tc) {
				tc->position = pos;
				tc->rotation = rot;
			}
		}
	}
}

/* ── Audio helpers ────────────────────────────────────────────────── */

static JceSound play_load_audio_clip(const char *clip_path)
{
	if (!s_play_audio || !clip_path || clip_path[0] == '\0')
		return JCE_SOUND_INVALID;

	const char *scene_path = jce_state_get_current_scene_path();
	char full[1024];
	if (scene_path && scene_path[0] != '\0') {
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

	const int entity_count = (int)g_entity_order.size();
	for (int i = 0; i < entity_count && s_play_voice_count < PLAY_MAX_VOICES; i++) {
		uint32_t id = g_entity_order[i];
		if (!jce_state_entity_enabled(id)) continue;

		JceEntity e = (JceEntity)id;
		JceAudioSourceComponent *as = jce_scene_get_audio_source(s.scene, e);
		if (!as || !as->play_on_awake) continue;

		JceSound snd = play_load_audio_clip(as->clip_path);
		if (snd == JCE_SOUND_INVALID) continue;

		JceVoice v = jce_audio_play(s_play_audio, snd,
		                            as->loop, as->volume, as->pitch);

		s_play_voices[s_play_voice_count].entity_id = id;
		s_play_voices[s_play_voice_count].sound     = snd;
		s_play_voices[s_play_voice_count].voice     = v;
		s_play_voice_count++;
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

	if (s_play_physics) {
		jce_physics_step(s_play_physics, dt);
		play_sync_physics_to_entities();
	}

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
	if (!jce_state_entity_exists(id)) return;

	const char *name = jce_state_entity_name(id);
	const char *tag  = jce_state_entity_tag(id);
	const char *pp   = jce_state_entity_prefab_path(id);

	s_clipboard.id = id;
	snprintf(s_clipboard.name, sizeof(s_clipboard.name), "%s", name ? name : "Entity");
	s_clipboard.tag_color = jce_state_entity_tag_color(id);
	snprintf(s_clipboard.tag, sizeof(s_clipboard.tag), "%s", tag ? tag : "");
	s_clipboard.prefab_instance = jce_state_entity_is_prefab(id);
	snprintf(s_clipboard.prefab_path, sizeof(s_clipboard.prefab_path),
	         "%s", pp ? pp : "");
	LOG_INFO(LOG_TAG, "copied entity %u (%s)", id, s_clipboard.name);
}

uint32_t jce_state_paste_entity(uint32_t parent_id)
{
	if (s_clipboard.id == 0) return 0;

	char paste_name[JCE_MAX_ENTITY_NAME];
	snprintf(paste_name, sizeof(paste_name), "%s (Paste)", s_clipboard.name);
	uint32_t new_id = jce_state_create_entity(paste_name, parent_id);
	if (new_id == 0) return 0;

	jce_state_set_entity_tag(new_id, s_clipboard.tag);
	jce_state_set_entity_tag_color(new_id, s_clipboard.tag_color);

	if (s_clipboard.prefab_instance && s.scene) {
		JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)new_id);
		if (m) {
			m->prefab_instance = true;
			snprintf(m->prefab_path, sizeof(m->prefab_path), "%s",
			         s_clipboard.prefab_path);
		}
	}

	LOG_INFO(LOG_TAG, "pasted entity as %u (%s)", new_id, paste_name);
	return new_id;
}

bool jce_state_has_copied(void)
{
	return s_clipboard.id != 0;
}
