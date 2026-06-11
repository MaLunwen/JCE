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
#include <jce/middleware/physics/jce_physics_layers.h>
#include <jce/middleware/physics/jce_physics_debug.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/renderer/jce_scene_renderer.h>   /* set_anim_sm_active */
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
}

#include "core/jce_assetdb.h"
#include "core/jce_project_settings.h"
#include "core/jce_editor_config.h"   /* jce_editor_dotjce_path (~/.jce) */

#include <string>

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

/* ── Contact statistics (Physics Debugger) ───────────────────────── */

static int s_active_contacts = 0;

static void editor_contact_cb(const JceContactEvent *ev, void * /*ud*/)
{
    if (!ev) return;
    if (ev->type == JCE_CONTACT_BEGIN)      ++s_active_contacts;
    else if (ev->type == JCE_CONTACT_END && s_active_contacts > 0)
        --s_active_contacts;
}

int jce_editor_play_get_active_contacts(void) { return s_active_contacts; }

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

    /* Project Settings → play session.  Pull the cached snapshot once and
     * govern this run with it (Time fixed timestep, Physics gravity, Audio
     * master mix).  RuntimeDesc has no audio field, so audio settings are
     * applied directly to the engine we just created — before the runtime
     * spawns its AudioSource voices below. */
    const JceProjectSettings *ps = jce_project_settings_current();

    if (s_play_audio && ps) {
        if (ps->audio.disable_audio) {
            jce_audio_set_master_volume(s_play_audio, 0.0f);
        } else {
            jce_audio_set_master_volume(s_play_audio, ps->audio.master_volume);
            jce_audio_set_doppler_factor(s_play_audio, ps->audio.doppler_factor);
        }
    }

    /* Bridge the project Layer Collision Matrix + layer names into the
     * engine's process-wide physics matrix.  jce_physics_body_set_layer
     * (applied per body at spawn inside jce_runtime_create below) reads
     * this engine matrix, so the push must happen first. */
    if (ps) {
        for (uint32_t i = 0; i < JCE_PS_LAYER_COUNT; ++i) {
            jce_physics_layer_set_name(i, ps->tags_layers.layers[i]);
            for (uint32_t j = i; j < JCE_PS_LAYER_COUNT; ++j) {
                bool collides =
                    (ps->physics.layer_collision_matrix[i] >> j) & 1u;
                jce_physics_set_layer_collides(i, j, collides);
            }
        }
    }

    JceRuntimeDesc rd;
    memset(&rd, 0, sizeof(rd));
    rd.scene          = s.scene;
    rd.pak            = NULL;                       /* editor uses host FS */
    rd.audio          = s_play_audio;
    rd.enable_physics = true;
    rd.audio_load_fn  = editor_play_audio_load;
    rd.user_data      = NULL;
    /* Govern the sim with Project Settings (Time / Physics).  Leave 0 for
     * any field the project doesn't override — jce_runtime_create falls
     * back to its own defaults (1/60 timestep, -9.81 gravity). */
    if (ps) {
        rd.fixed_timestep = ps->time.fixed_timestep;
        rd.gravity_y      = ps->physics.gravity[1];
    }
    /* Seed the runtime mixer from the same audio_mixer.json the Audio Mixer
     * panel writes (~/.jce), so Music/SFX/Voice slider edits drive Play-mode
     * bus volumes.  Missing file => runtime falls back to default buses. */
    static char s_mixer_cfg[1024];
    if (jce_editor_dotjce_path("audio_mixer.json", s_mixer_cfg,
                               sizeof(s_mixer_cfg)))
        rd.mixer_config_path = s_mixer_cfg;
    /* Navmesh: hand the runtime the .navmesh.bin baked by the NavMesh panel
     * as a sibling of the current scene (same basename, .navmesh.bin).  The
     * runtime loads it + stands up the nav-agent set; missing file disables
     * navigation. */
    static char s_navmesh_bin[1024];
    const char *play_scene_path = jce_state_get_current_scene_path();
    if (play_scene_path && play_scene_path[0]) {
        std::string np = play_scene_path;
        const char *sfxs[] = { ".scene.json", ".json" };
        for (const char *sfx : sfxs) {
            size_t sl = strlen(sfx);
            if (np.size() >= sl && np.compare(np.size() - sl, sl, sfx) == 0) {
                np.erase(np.size() - sl);
                break;
            }
        }
        np += ".navmesh.bin";
        if (jce_fs_host_exists_file(np.c_str())) {
            snprintf(s_navmesh_bin, sizeof(s_navmesh_bin), "%s", np.c_str());
            rd.navmesh_path = s_navmesh_bin;
        }
    }
    /* Save snapshots: write SavePoint checkpoints to "<project>/saves" — the
     * same directory the Save Browser panel scans.  Missing project => leave
     * empty (the runtime still registers the snapshot provider). */
    static char s_saves_dir[1024];
    const char *proj_root = jce_editor_assets_get_project();
    if (proj_root && proj_root[0]) {
        snprintf(s_saves_dir, sizeof(s_saves_dir), "%s/saves", proj_root);
        rd.saves_dir = s_saves_dir;
    }
    s_play_runtime    = jce_runtime_create(&rd);
    if (!s_play_runtime) {
        LOG_ERROR(LOG_TAG, "failed to create play runtime");
        if (s_play_audio) { jce_audio_destroy(s_play_audio); s_play_audio = NULL; }
        return;
    }

    /* Subscribe to contact events so the Physics Debugger can show live
     * active-contact counts (also activates engine manifold diffing). */
    s_active_contacts = 0;
    jce_runtime_set_contact_listener(s_play_runtime, editor_contact_cb, NULL);

    jce_editor_scene_reset_anim_timer();
    /* Let bound animation state machines own active_clip while playing (in the
     * editor they stay idle so manual clip preview keeps working). */
    jce_scene_renderer_set_anim_sm_active(jce_editor_get_scene_renderer(), true);
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
    s_active_contacts = 0;

    if (s_play_snapshot_valid) {
        history_restore_snapshot(s_play_snapshot, "play-stop-restore");
        s_play_snapshot       = EditorHistorySnapshot();
        s_play_snapshot_valid = false;
    }

    s.play_state = JCE_PLAY_STOPPED;
    jce_editor_scene_reset_anim_timer();
    /* Back to editor preview: SM idle, manual clip selection previews again. */
    jce_scene_renderer_set_anim_sm_active(jce_editor_get_scene_renderer(), false);
    LOG_INFO(LOG_TAG, "play mode stopped");
}

JcePlayState jce_state_get_play_state(void) { return s.play_state; }

void stop_play_before_scene_swap(void)
{
    if (jce_state_get_play_state() != JCE_PLAY_STOPPED) {
        LOG_INFO(LOG_TAG, "auto-stopping play mode before scene swap "
                          "(new/open/load during Play)");
        jce_state_stop();
    }
}

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
                                      bool jump_pressed, bool jump_held,
                                      bool sprint)
{
    if (!s_play_runtime) return;
    JceRuntimeInput in = {};
    in.walk_x       = walk_x;
    in.walk_z       = walk_z;
    in.jump_pressed = jump_pressed;
    in.jump_held    = jump_held;
    in.sprint       = sprint;
    in.speed_mult   = 1.0f;   /* sprint scaling is authored on the component */
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

JceRuntime *jce_editor_play_get_runtime(void)
{
    return s_play_runtime;
}

/* ── Entity Clipboard ────────────────────────────────────────────── */

struct ClipEntry {
    uint32_t    id;
    char        name[JCE_MAX_ENTITY_NAME];
    /* Full subtree snapshot (entity tree node JSON: name/components/children).
     * Captured via serialize_entity_tree_json() so copy/paste preserves the
     * entire child hierarchy and every component type, not just a flat
     * component array on a single entity. */
    std::string tree_json;
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
    out->id = id;
    snprintf(out->name, sizeof(out->name), "%s", name ? name : "Entity");
    out->tree_json.clear();

    if (s.scene) {
        /* Serialize the full subtree (root + transitive children + every
         * component type) — single source of truth for prefab/scene save. */
        JceJson *node = serialize_entity_tree_json(id);
        if (node) {
            char *txt = jce_json_print(node, false);
            if (txt) {
                out->tree_json.assign(txt);
                jce_json_free_string(txt);
            }
            jce_json_free(node);
        }
    }
    return true;
}

static uint32_t clip_paste_one(const ClipEntry &e, uint32_t parent_id,
                                const char *name_suffix)
{
    if (!s.scene || e.tree_json.empty()) return 0;

    JceJson *node = jce_json_parse(e.tree_json.c_str(), e.tree_json.size());
    if (!node) return 0;

    /* Apply the paste suffix to the root node's name before rebuilding. */
    if (name_suffix && name_suffix[0] != '\0') {
        const char *base = jce_json_get_string(node, "name", e.name);
        char paste_name[JCE_MAX_ENTITY_NAME];
        snprintf(paste_name, sizeof(paste_name), "%s%s",
                 base ? base : e.name, name_suffix);
        jce_json_set_string(node, "name", paste_name);
    }

    /* Rebuild the entire subtree (root + children + all components). */
    uint32_t new_id = load_entity_tree_node(node, parent_id);
    jce_json_free(node);
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

/* True if `id` has any ancestor that is itself present in the selection.
 * Such an entity is already captured inside its ancestor's subtree, so
 * capturing it again would paste a duplicate. */
static bool clip_id_in_set(const uint32_t *ids, int count, uint32_t id)
{
    for (int i = 0; i < count; i++)
        if (ids[i] == id) return true;
    return false;
}

static bool clip_has_selected_ancestor(const uint32_t *ids, int count,
                                        uint32_t id)
{
    uint32_t p = jce_state_entity_parent(id);
    while (p != 0) {
        if (clip_id_in_set(ids, count, p)) return true;
        p = jce_state_entity_parent(p);
    }
    return false;
}

void jce_state_copy_entities(const uint32_t *ids, int count, bool cut)
{
    s_clip_entries.clear();
    s_clip_cut = false;
    s_clipboard.id = 0;
    if (!ids || count <= 0) return;
    s_clip_entries.reserve((size_t)count);
    for (int i = 0; i < count; i++) {
        /* Skip entities already nested under another selected entity —
         * subtree capture would otherwise duplicate them. */
        if (clip_has_selected_ancestor(ids, count, ids[i]))
            continue;
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
