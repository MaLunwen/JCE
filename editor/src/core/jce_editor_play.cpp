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
#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/scene/jce_scene.h>
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

struct ClipEntry {
	uint32_t    id;
	char        name[JCE_MAX_ENTITY_NAME];
	JceTagColor tag_color;
	char        tag[JCE_MAX_TAG_STRING];
	bool        prefab_instance;
	char        prefab_path[JCE_MAX_PREFAB_PATH];
};

#include <vector>
static std::vector<ClipEntry> s_clip_entries;
static bool                   s_clip_cut = false;

/* Single-entity legacy fields kept zero-initialised so existing callers of
 * jce_state_has_copied() still observe the multi clipboard. */
static struct {
	uint32_t    id;
} s_clipboard;

static bool clip_capture(uint32_t id, ClipEntry *out)
{
	if (!jce_state_entity_exists(id)) return false;
	const char *name = jce_state_entity_name(id);
	const char *tag  = jce_state_entity_tag(id);
	const char *pp   = jce_state_entity_prefab_path(id);
	out->id = id;
	snprintf(out->name, sizeof(out->name), "%s", name ? name : "Entity");
	out->tag_color = jce_state_entity_tag_color(id);
	snprintf(out->tag, sizeof(out->tag), "%s", tag ? tag : "");
	out->prefab_instance = jce_state_entity_is_prefab(id);
	snprintf(out->prefab_path, sizeof(out->prefab_path), "%s", pp ? pp : "");
	return true;
}

static uint32_t clip_paste_one(const ClipEntry &e, uint32_t parent_id,
                                const char *name_suffix)
{
	char paste_name[JCE_MAX_ENTITY_NAME];
	snprintf(paste_name, sizeof(paste_name), "%s%s",
	         e.name, name_suffix ? name_suffix : "");
	uint32_t new_id = jce_state_create_entity(paste_name, parent_id);
	if (new_id == 0) return 0;

	jce_state_set_entity_tag(new_id, e.tag);
	jce_state_set_entity_tag_color(new_id, e.tag_color);

	if (e.prefab_instance && s.scene) {
		JceEditorMeta *m = jce_scene_get_editor_meta(s.scene, (JceEntity)new_id);
		if (m) {
			m->prefab_instance = true;
			snprintf(m->prefab_path, sizeof(m->prefab_path), "%s",
			         e.prefab_path);
		}
	}
	return new_id;
}

void jce_state_copy_entity(uint32_t id)
{
	ClipEntry e;
	if (!clip_capture(id, &e)) return;
	s_clip_entries.clear();
	s_clip_entries.push_back(e);
	s_clip_cut = false;
	s_clipboard.id = id;
	LOG_INFO(LOG_TAG, "copied entity %u (%s)", id, e.name);
}

uint32_t jce_state_paste_entity(uint32_t parent_id)
{
	if (s_clip_entries.empty()) return 0;
	uint32_t new_id = clip_paste_one(s_clip_entries[0], parent_id, " (Paste)");
	if (new_id) LOG_INFO(LOG_TAG, "pasted entity as %u", new_id);
	if (s_clip_cut && new_id) {
		jce_state_delete_entity(s_clip_entries[0].id);
		s_clip_entries.clear();
		s_clip_cut = false;
		s_clipboard.id = 0;
	}
	return new_id;
}

bool jce_state_has_copied(void)
{
	return !s_clip_entries.empty();
}

void jce_state_copy_entities(const uint32_t *ids, int count, bool cut)
{
	s_clip_entries.clear();
	s_clip_cut = false;
	s_clipboard.id = 0;
	if (!ids || count <= 0) return;
	s_clip_entries.reserve((size_t)count);
	for (int i = 0; i < count; i++) {
		ClipEntry e;
		if (clip_capture(ids[i], &e))
			s_clip_entries.push_back(e);
	}
	s_clip_cut = cut;
	if (!s_clip_entries.empty()) {
		s_clipboard.id = s_clip_entries[0].id;
		LOG_INFO(LOG_TAG, "%s %d entit%s",
		         cut ? "cut" : "copied",
		         (int)s_clip_entries.size(),
		         s_clip_entries.size() == 1 ? "y" : "ies");
	}
}

int jce_state_clipboard_count(void)
{
	return (int)s_clip_entries.size();
}

bool jce_state_clipboard_is_cut(void)
{
	return s_clip_cut && !s_clip_entries.empty();
}

const uint32_t *jce_state_clipboard_source_ids(int *out_count)
{
	if (out_count) *out_count = (int)s_clip_entries.size();
	if (s_clip_entries.empty()) return NULL;
	/* Build a stable scratch array. */
	static std::vector<uint32_t> scratch;
	scratch.clear();
	scratch.reserve(s_clip_entries.size());
	for (const auto &e : s_clip_entries) scratch.push_back(e.id);
	return scratch.data();
}

int jce_state_paste_entities(uint32_t parent_id,
                             uint32_t *out_ids, int max_out)
{
	if (s_clip_entries.empty()) return 0;
	int n = 0;
	jce_state_begin_batch_edit();
	for (const auto &e : s_clip_entries) {
		const char *suffix = s_clip_cut ? "" : " (Paste)";
		uint32_t new_id = clip_paste_one(e, parent_id, suffix);
		if (new_id == 0) continue;
		if (out_ids && n < max_out) out_ids[n] = new_id;
		++n;
	}
	if (s_clip_cut) {
		for (const auto &e : s_clip_entries)
			jce_state_delete_entity(e.id);
	}
	jce_state_end_batch_edit();
	if (s_clip_cut) {
		s_clip_entries.clear();
		s_clip_cut = false;
		s_clipboard.id = 0;
	}
	LOG_INFO(LOG_TAG, "pasted %d entities under %u", n, parent_id);
	return n;
}
