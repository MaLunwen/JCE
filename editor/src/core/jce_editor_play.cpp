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
#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
}

#include "core/jce_assetdb.h"

/* ── Play mode static data ───────────────────────────────────────── */

static EditorHistorySnapshot s_play_snapshot;
static bool s_play_snapshot_valid = false;

static JcePhysicsWorld *s_play_physics = NULL;

#define PLAY_MAX_BODIES 256
static struct {
    int entity_index; /* index into g_entity_order */
    JceBodyHandle body;
} s_play_bodies[PLAY_MAX_BODIES];
static int s_play_body_count = 0;

/* Player character (only one supported per scene). */
static JceCharacterHandle s_play_character    = { UINT32_MAX };
static int                s_play_player_eidx  = -1;
static struct {
    float walk_x, walk_z;
    bool  jump_pressed;
    float speed_mult;
} s_play_player_input = { 0.0f, 0.0f, false, 1.0f };

static JceAudio *s_play_audio = NULL;

#define PLAY_MAX_VOICES 64
static struct {
    uint32_t entity_id;
    JceSound sound;
    JceVoice voice;
    bool     spatial;
} s_play_voices[PLAY_MAX_VOICES];
static int s_play_voice_count = 0;

/* Compute world-space position of `entity` by summing local positions
 * up the parent chain (rotation/scale ignored — sufficient for audio
 * listener distance attenuation). */
static jce_vec3 play_entity_world_position(JceScene *scene, uint32_t id)
{
    jce_vec3 acc = { 0.0f, 0.0f, 0.0f };
    while (id != 0 && jce_state_entity_exists(id)) {
        JceTransform *tc = jce_scene_get_transform(scene, (JceEntity)id);
        if (tc) {
            acc.x += tc->position.x;
            acc.y += tc->position.y;
            acc.z += tc->position.z;
        }
        id = jce_state_entity_parent(id);
    }
    return acc;
}

/* ── Physics helpers ─────────────────────────────────────────────── */

static void play_create_physics_world(void)
{
    JcePhysicsWorldDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.gravity.x = 0.0f;
    desc.gravity.y = -9.81f;
    desc.gravity.z = 0.0f;
    desc.fixed_timestep = 1.0f / 60.0f;
    s_play_physics      = jce_physics_create(&desc);
    s_play_body_count   = 0;
    s_play_character.idx = UINT32_MAX;
    s_play_player_eidx  = -1;
    s_play_player_input.walk_x       = 0.0f;
    s_play_player_input.walk_z       = 0.0f;
    s_play_player_input.jump_pressed = false;
    s_play_player_input.speed_mult   = 1.0f;

    if (!s_play_physics) {
        LOG_ERROR(LOG_TAG, "failed to create physics world for play mode");
        return;
    }

    const int entity_count = (int)g_entity_order.size();
    for (int i = 0; i < entity_count && s_play_body_count < PLAY_MAX_BODIES; i++) {
        uint32_t id = g_entity_order[i];
        if (!jce_state_entity_enabled(id)) continue;

        JceEntity     e  = (JceEntity)id;
        JceTransform *tf = jce_scene_get_transform(s.scene, e);
        if (!tf) continue;

        /* CharacterController takes precedence — no rigid body. */
        JceCharacterControllerComponent *cc =
            jce_scene_get_character_controller(s.scene, e);
        if (cc && !jce_character_valid(s_play_character)) {
            JceCharacterDesc cd;
            memset(&cd, 0, sizeof(cd));
            cd.position      = tf->position;
            cd.radius        = cc->radius      > 0.0f ? cc->radius      : 0.35f;
            cd.height        = cc->height      > 0.0f ? cc->height      : 1.8f;
            cd.step_height   = cc->step_offset > 0.0f ? cc->step_offset : 0.35f;
            cd.max_slope_deg = cc->slope_limit > 0.0f ? cc->slope_limit : 50.0f;
            cd.gravity       = 9.81f;
            cd.jump_speed    = 5.0f;
            s_play_character   = jce_physics_character_create(s_play_physics, &cd);
            s_play_player_eidx = i;
            continue;
        }

        JceRigidBodyComponent *rb = jce_scene_get_rigidbody(s.scene, e);
        if (!rb) continue;

        JceBodyDesc bd;
        memset(&bd, 0, sizeof(bd));
        bd.position        = tf->position;
        bd.rotation        = jce_q_identity();
        bd.mass            = rb->mass;
        bd.linear_damping  = rb->drag;
        bd.angular_damping = rb->angular_drag;
        bd.friction        = rb->friction    > 0.0f ? rb->friction    : 0.5f;
        bd.restitution     = rb->restitution;

        /* Pick collider shape from whichever component the entity has. */
        JceBoxColliderComponent    *box = jce_scene_get_box_collider(s.scene, e);
        JceSphereColliderComponent *sph = jce_scene_get_sphere_collider(s.scene, e);
        if (box) {
            bd.shape = JCE_SHAPE_BOX;
            /* size = full extents; account for entity scale */
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
            float scale_max = tf->scale.x;
            if (tf->scale.y > scale_max) scale_max = tf->scale.y;
            if (tf->scale.z > scale_max) scale_max = tf->scale.z;
            float r = sph->radius > 0.0f ? sph->radius : 0.5f;
            bd.half_extents.x = r * scale_max;
            bd.position.x += sph->center[0];
            bd.position.y += sph->center[1];
            bd.position.z += sph->center[2];
            bd.is_trigger = sph->is_trigger;
        } else {
            /* No collider authored: fall back to a tiny placeholder box
             * so dropped objects still collide with the world. */
            bd.shape          = JCE_SHAPE_BOX;
            bd.half_extents.x = 0.5f * tf->scale.x;
            bd.half_extents.y = 0.5f * tf->scale.y;
            bd.half_extents.z = 0.5f * tf->scale.z;
            if (bd.half_extents.x <= 0.0f) bd.half_extents.x = 0.5f;
            if (bd.half_extents.y <= 0.0f) bd.half_extents.y = 0.5f;
            if (bd.half_extents.z <= 0.0f) bd.half_extents.z = 0.5f;
        }

        if (rb->is_kinematic)
            bd.type = JCE_BODY_KINEMATIC;
        else if (rb->mass <= 0.0f)
            bd.type = JCE_BODY_STATIC;
        else
            bd.type = JCE_BODY_DYNAMIC;

        JceBodyHandle body = jce_physics_body_create(s_play_physics, &bd);
        if (jce_body_valid(body)) {
            /* Apply Continuous Collision Detection settings (P3-C.3). */
            if (rb->ccd_mode != JCE_CCD_DISCRETE) {
                jce_physics_body_set_ccd_mode(s_play_physics, body,
                                              (JceCcdMode)rb->ccd_mode);
                if (rb->ccd_threshold > 0.0f) {
                    jce_physics_body_set_ccd_motion_threshold(
                        s_play_physics, body, rb->ccd_threshold);
                }
                if (rb->ccd_sphere_radius > 0.0f) {
                    jce_physics_body_set_ccd_swept_sphere_radius(
                        s_play_physics, body, rb->ccd_sphere_radius);
                }
            }
            s_play_bodies[s_play_body_count].entity_index = i;
            s_play_bodies[s_play_body_count].body         = body;
            s_play_body_count++;
        }
    }

    LOG_INFO(LOG_TAG,
             "play physics: %d bodies, character=%s",
             s_play_body_count,
             jce_character_valid(s_play_character) ? "yes" : "no");
}

static void play_destroy_physics_world(void)
{
    if (s_play_physics) {
        if (jce_character_valid(s_play_character))
            jce_physics_character_destroy(s_play_physics, s_play_character);
        jce_physics_destroy(s_play_physics);
        s_play_physics = NULL;
    }
    s_play_character.idx = UINT32_MAX;
    s_play_player_eidx  = -1;
    s_play_body_count   = 0;
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

    /* Sync player character. */
    if (jce_character_valid(s_play_character) &&
        s_play_player_eidx >= 0 && s_play_player_eidx < entity_count &&
        s.scene) {
        jce_vec3 cpos;
        jce_physics_character_get_position(s_play_physics, s_play_character, &cpos);
        uint32_t id = g_entity_order[s_play_player_eidx];
        if (id != 0) {
            JceTransform *tc = jce_scene_get_transform(s.scene, (JceEntity)id);
            if (tc) tc->position = cpos;
        }
    }
}

static void play_drive_character(float dt)
{
    if (!s_play_physics || !jce_character_valid(s_play_character)) return;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;

    float mult = s_play_player_input.speed_mult;
    jce_vec3 walk = jce_v3(s_play_player_input.walk_x * mult,
                           0.0f,
                           s_play_player_input.walk_z * mult);
    jce_physics_character_move(s_play_physics, s_play_character, walk, dt);
    if (s_play_player_input.jump_pressed) {
        if (jce_physics_character_is_grounded(s_play_physics, s_play_character))
            jce_physics_character_jump(s_play_physics, s_play_character);
        s_play_player_input.jump_pressed = false;
    }
}

void jce_editor_play_set_player_input(float walk_x, float walk_z,
                                       bool jump_pressed, float speed_mult)
{
    s_play_player_input.walk_x     = walk_x;
    s_play_player_input.walk_z     = walk_z;
    if (jump_pressed) s_play_player_input.jump_pressed = true;
    s_play_player_input.speed_mult = speed_mult;
}

bool jce_editor_play_get_player_position(float *out_x, float *out_y, float *out_z)
{
    if (!s_play_physics || !jce_character_valid(s_play_character)) return false;
    jce_vec3 p;
    jce_physics_character_get_position(s_play_physics, s_play_character, &p);
    if (out_x) *out_x = p.x;
    if (out_y) *out_y = p.y;
    if (out_z) *out_z = p.z;
    return true;
}

JcePhysicsWorld *jce_editor_play_get_physics_world(void)
{
    return s_play_physics;
}

/* ── Audio helpers ────────────────────────────────────────────────── */

static JceSound play_load_audio_clip(const char *clip_path)
{
    if (!s_play_audio || !clip_path || clip_path[0] == '\0')
        return JCE_SOUND_INVALID;

    /* Build a list of candidate roots to try (Unity-parity: clipPath is
     * relative to the project's assets root, but we also fall back to
     * scene-dir-relative and walk up a few levels to be forgiving for
     * scenes opened outside a project context). */
    char candidates[8][1024];
    int  cand_n = 0;

    /* VFS-first: when an active bundle is mounted (scene opened from a
     * .jbundle) the clip lives at its virtual path unprefixed.  Try the
     * raw clip_path before any disk roots so jce_fs_host_read_all hits
     * the bundle reader instead of probing the project tree. */
    snprintf(candidates[cand_n++], sizeof(candidates[0]), "%s", clip_path);

    const char *project_root = jce_assetdb_get_root();
    if (project_root && project_root[0] != '\0' && cand_n < 8) {
        snprintf(candidates[cand_n++], sizeof(candidates[0]), "%s/%s",
                 project_root, clip_path);
    }

    char scene_dir[512] = {0};
    const char *scene_path = jce_state_get_current_scene_path();
    if (scene_path && scene_path[0] != '\0') {
        snprintf(scene_dir, sizeof(scene_dir), "%s", scene_path);
        char *sep = strrchr(scene_dir, '/');
        char *bsep = strrchr(scene_dir, '\\');
        if (bsep && (!sep || bsep > sep)) sep = bsep;
        if (sep) *sep = '\0';
        else scene_dir[0] = '\0';

        if (scene_dir[0] && cand_n < 8) {
            snprintf(candidates[cand_n++], sizeof(candidates[0]), "%s/%s",
                     scene_dir, clip_path);
        }

        /* Walk up scene_dir up to 5 levels; project layouts commonly put
         * scenes in `assets/scenes/` while audio lives in `assets/musics/`,
         * so the file is one level above the scene. */
        char up[512];
        snprintf(up, sizeof(up), "%s", scene_dir);
        for (int level = 0; level < 5 && cand_n < 8; ++level) {
            char *sep1 = strrchr(up, '/');
            char *bsep1 = strrchr(up, '\\');
            if (bsep1 && (!sep1 || bsep1 > sep1)) sep1 = bsep1;
            if (!sep1) break;
            *sep1 = '\0';
            if (up[0] == '\0') break;
            snprintf(candidates[cand_n++], sizeof(candidates[0]), "%s/%s",
                     up, clip_path);
        }
    }

    if (cand_n == 0) {
        snprintf(candidates[cand_n++], sizeof(candidates[0]), "%s", clip_path);
    }

    char     full[1024] = {0};
    void    *data       = NULL;
    uint64_t fsize      = 0;
    for (int i = 0; i < cand_n; ++i) {
        data = jce_fs_host_read_all(candidates[i], &fsize);
        if (data && fsize > 0) {
            snprintf(full, sizeof(full), "%s", candidates[i]);
            break;
        }
        if (data) { jce_free(data); data = NULL; fsize = 0; }
    }

    if (!data || fsize == 0) {
        LOG_WARN(LOG_TAG, "play audio: could not read '%s' (tried %d roots)",
                 clip_path, cand_n);
        if (data) jce_free(data);
        return JCE_SOUND_INVALID;
    }

    JceSound snd = jce_audio_load_memory(s_play_audio, data, (uint32_t)fsize, clip_path);
    jce_free(data);
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

        bool spatial = (as->spatial_blend > 0.5f);
        if (spatial) {
            jce_audio_voice_set_3d(s_play_audio, v, true);
            jce_vec3 wp = play_entity_world_position(s.scene, id);
            jce_audio_voice_set_position(s_play_audio, v, wp.x, wp.y, wp.z);
            /* Defaults tuned for a hand-placed showcase: full volume
             * within 1 m, audible roll-off out to ~25 m. */
            jce_audio_voice_set_attenuation(s_play_audio, v,
                                            JCE_AUDIO_ATTEN_INVERSE,
                                            1.0f, 25.0f, 1.0f);
        } else {
            /* miniaudio enables spatialization by default; force it off
             * so 2D sources (spatialBlend=0) play uniformly regardless
             * of listener position — matches Unity's 2D AudioSource. */
            jce_audio_voice_set_3d(s_play_audio, v, false);
        }

        s_play_voices[s_play_voice_count].entity_id = id;
        s_play_voices[s_play_voice_count].sound     = snd;
        s_play_voices[s_play_voice_count].voice     = v;
        s_play_voices[s_play_voice_count].spatial   = spatial;
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

/* Per-frame: keep the listener attached to the primary camera and
 * update each spatial source's world position so moving sources (or
 * a moving listener) actually exhibit distance attenuation / panning. */
static void play_update_audio_3d(void)
{
    if (!s_play_audio || !s.scene) return;

    /* Listener := primary camera entity's world transform. */
    JceAudioListener listener;
    listener.position[0] = listener.position[1] = listener.position[2] = 0.0f;
    listener.forward[0]  = 0.0f; listener.forward[1] = 0.0f; listener.forward[2] = -1.0f;
    listener.up[0]       = 0.0f; listener.up[1]      = 1.0f; listener.up[2]      = 0.0f;
    listener.velocity[0] = listener.velocity[1] = listener.velocity[2] = 0.0f;

    int ecount = (int)g_entity_order.size();
    for (int i = 0; i < ecount; i++) {
        uint32_t id = g_entity_order[i];
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        JceCameraComponent *cam = jce_scene_get_camera(s.scene, (JceEntity)id);
        if (!cam || !cam->is_primary) continue;
        jce_vec3 wp = play_entity_world_position(s.scene, id);
        listener.position[0] = wp.x;
        listener.position[1] = wp.y;
        listener.position[2] = wp.z;
        break;
    }
    jce_audio_set_listener(s_play_audio, &listener);

    /* Push current world positions to every spatial voice. */
    for (int i = 0; i < s_play_voice_count; i++) {
        if (!s_play_voices[i].spatial) continue;
        jce_vec3 wp = play_entity_world_position(s.scene,
                                                  s_play_voices[i].entity_id);
        jce_audio_voice_set_position(s_play_audio, s_play_voices[i].voice,
                                      wp.x, wp.y, wp.z);
    }
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

void jce_state_step(float dt)
{
    if (s.play_state != JCE_PLAY_PAUSED) return;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;

    if (s_play_physics) {
        play_drive_character(dt);
        jce_physics_step(s_play_physics, dt);
        play_sync_physics_to_entities();
    }
    if (s.scene)
        jce_scene_update(s.scene, dt);
}

void jce_state_play_mode_tick(float dt)
{
    if (s.play_state != JCE_PLAY_PLAYING) return;

    if (s_play_physics) {
        play_drive_character(dt);
        jce_physics_step(s_play_physics, dt);
        play_sync_physics_to_entities();
    }

    if (s.scene)
        jce_scene_update(s.scene, dt);

    play_update_audio_3d();
}

/* ── Entity Clipboard ────────────────────────────────────────────── */

struct ClipEntry {
    uint32_t    id;
    char        name[JCE_MAX_ENTITY_NAME];
    JceTagColor tag_color;
    char        tag[JCE_MAX_TAG_STRING];
    bool        prefab_instance;
    char        prefab_path[JCE_MAX_PREFAB_PATH];
    std::string components_json;     /* serialized component array, may be empty */
};

#include <vector>
#include <string>
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
    out->components_json.clear();

    if (s.scene) {
        JceJson *arr = jce_scene_serialize_entity_components(s.scene, (JceEntity)id);
        if (arr) {
            char *txt = jce_json_print(arr, false);
            if (txt) {
                out->components_json.assign(txt);
                jce_json_free_string(txt);
            }
            jce_json_free(arr);
        }
    }
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

    if (s.scene && !e.components_json.empty()) {
        JceJson *arr = jce_json_parse(e.components_json.c_str(),
                                      e.components_json.size());
        if (arr) {
            JceJson *wrapper = jce_json_object();
            if (wrapper) {
                jce_json_set_child(wrapper, "components", arr);
                jce_scene_parse_entity_json(s.scene,
                                            (JceEntity)new_id, wrapper);
                jce_json_free(wrapper); /* frees nested arr too */
            } else {
                jce_json_free(arr);
            }
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
