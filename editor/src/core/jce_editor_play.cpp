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
#include "ui/jce_editor_panels.h"

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

/* ── Play mode: runtime-driven ───────────────────────────────────── */

#include <jce/application/jce_runtime.h>

static EditorHistorySnapshot s_play_snapshot;
static bool                  s_play_snapshot_valid = false;

static JceRuntime *s_play_runtime = NULL;
static JceAudio   *s_play_audio   = NULL;   /* owned alongside the runtime */

/* Editor-only audio resolver: probe a few host-FS roots (VFS bundle path,
 * project root, scene-dir, and a few parents) before giving up.  Matches
 * the legacy editor Play behaviour so showcase scenes that put music
 * next to assets/ still load when opened outside a project context.    */
extern "C" {
#include <jce/os/core/jce_filesystem.h>
}

static uint32_t editor_play_audio_load(void * /*ud*/, JceAudio *audio,
                                       const char *clip_path)
{
    if (!audio || !clip_path || !clip_path[0]) return JCE_SOUND_INVALID;

    char candidates[8][1024];
    int  cand_n = 0;

    /* VFS first (mounted bundle vpath). */
    snprintf(candidates[cand_n++], sizeof(candidates[0]), "%s", clip_path);

    const char *project_root = jce_assetdb_get_root();
    if (project_root && project_root[0] && cand_n < 8) {
        snprintf(candidates[cand_n++], sizeof(candidates[0]), "%s/%s",
                 project_root, clip_path);
    }

    char scene_dir[512] = {0};
    const char *scene_path = jce_state_get_current_scene_path();
    if (scene_path && scene_path[0]) {
        snprintf(scene_dir, sizeof(scene_dir), "%s", scene_path);
        jce_editor_path_trim_to_parent(scene_dir);
        if (scene_dir[0] && cand_n < 8) {
            snprintf(candidates[cand_n++], sizeof(candidates[0]), "%s/%s",
                     scene_dir, clip_path);
        }
        char up[512];
        snprintf(up, sizeof(up), "%s", scene_dir);
        for (int level = 0; level < 5 && cand_n < 8; ++level) {
            jce_editor_path_trim_to_parent(up);
            if (!up[0]) break;
            snprintf(candidates[cand_n++], sizeof(candidates[0]), "%s/%s",
                     up, clip_path);
        }
    }

    void    *data  = NULL;
    uint64_t fsize = 0;
    for (int i = 0; i < cand_n; ++i) {
        data = jce_fs_host_read_all(candidates[i], &fsize);
        if (data && fsize > 0) break;
        if (data) { jce_free(data); data = NULL; fsize = 0; }
    }
    if (!data || fsize == 0) {
        LOG_WARN(LOG_TAG, "play audio: could not read '%s' (tried %d roots)",
                 clip_path, cand_n);
        if (data) jce_free(data);
        return JCE_SOUND_INVALID;
    }

    JceSound snd = jce_audio_load_memory(audio, data, (uint32_t)fsize, clip_path);
    jce_free(data);
    return snd;
}

/* ── Play mode API ───────────────────────────────────────────────── */

void jce_state_play(void)
{
    if (s.play_state != JCE_PLAY_STOPPED) return;

    s_play_snapshot_valid = history_capture_snapshot(&s_play_snapshot);
    if (!s_play_snapshot_valid)
        LOG_WARN(LOG_TAG, "failed to capture play-mode snapshot");

    s_play_audio = jce_audio_create();
    if (!s_play_audio)
        LOG_WARN(LOG_TAG, "failed to create audio engine for play mode");

    JceRuntimeDesc rd;
    memset(&rd, 0, sizeof(rd));
    rd.scene          = s.scene;
    rd.pak            = NULL;                       /* editor uses host FS */
    rd.audio          = s_play_audio;
    rd.enable_physics = true;
    rd.audio_load_fn  = editor_play_audio_load;
    rd.user_data      = NULL;
    s_play_runtime    = jce_runtime_create(&rd);
    if (!s_play_runtime) {
        LOG_ERROR(LOG_TAG, "failed to create play runtime");
        if (s_play_audio) { jce_audio_destroy(s_play_audio); s_play_audio = NULL; }
        return;
    }

    jce_editor_scene_reset_anim_timer();
    s.play_state = JCE_PLAY_PLAYING;
    LOG_INFO(LOG_TAG, "play mode started");
}

void jce_state_pause(void)
{
    if (s.play_state == JCE_PLAY_PLAYING) {
        if (s_play_runtime) jce_runtime_pause_audio(s_play_runtime);
        s.play_state = JCE_PLAY_PAUSED;
        LOG_INFO(LOG_TAG, "play mode paused");
    } else if (s.play_state == JCE_PLAY_PAUSED) {
        if (s_play_runtime) jce_runtime_resume_audio(s_play_runtime);
        s.play_state = JCE_PLAY_PLAYING;
        LOG_INFO(LOG_TAG, "play mode resumed");
    }
}

void jce_state_stop(void)
{
    if (s.play_state == JCE_PLAY_STOPPED) return;

    if (s_play_runtime) { jce_runtime_destroy(s_play_runtime); s_play_runtime = NULL; }
    if (s_play_audio)   { jce_audio_destroy(s_play_audio);     s_play_audio   = NULL; }

    if (s_play_snapshot_valid) {
        history_restore_snapshot(s_play_snapshot, "play-stop-restore");
        s_play_snapshot       = EditorHistorySnapshot();
        s_play_snapshot_valid = false;
    }

    s.play_state = JCE_PLAY_STOPPED;
    jce_editor_scene_reset_anim_timer();
    LOG_INFO(LOG_TAG, "play mode stopped");
}

JcePlayState jce_state_get_play_state(void) { return s.play_state; }

void jce_state_step(float dt)
{
    if (s.play_state != JCE_PLAY_PAUSED) return;
    if (s_play_runtime) jce_runtime_step(s_play_runtime, dt > 0 ? dt : 1.0f/60.0f);
}

void jce_state_play_mode_tick(float dt)
{
    if (s.play_state != JCE_PLAY_PLAYING) return;
    if (s_play_runtime) jce_runtime_step(s_play_runtime, dt);
}

/* ── External hooks (game view / scene render) ──────────────────── */

void jce_editor_play_set_player_input(float walk_x, float walk_z,
                                      bool jump_pressed, float speed_mult)
{
    if (!s_play_runtime) return;
    JceRuntimeInput in;
    in.walk_x       = walk_x;
    in.walk_z       = walk_z;
    in.jump_pressed = jump_pressed;
    in.speed_mult   = speed_mult > 0.0f ? speed_mult : 1.0f;
    jce_runtime_set_input(s_play_runtime, &in);
}

bool jce_editor_play_get_player_position(float *out_x, float *out_y, float *out_z)
{
    if (!s_play_runtime) return false;
    jce_vec3 p;
    if (!jce_runtime_get_player_position(s_play_runtime, &p)) return false;
    if (out_x) *out_x = p.x;
    if (out_y) *out_y = p.y;
    if (out_z) *out_z = p.z;
    return true;
}

JcePhysicsWorld *jce_editor_play_get_physics_world(void)
{
    return s_play_runtime ? jce_runtime_physics(s_play_runtime) : NULL;
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
