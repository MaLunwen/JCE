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
#include "scene/jce_editor_scene_asset_cache.h" /* resolve_mesh_path (collider) */
#include "ui/jce_editor_panels.h"

extern "C" {
#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/physics/jce_physics_layers.h>
#include <jce/middleware/physics/jce_physics_debug.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/resource/jce_world_streamer.h>   /* editor-Play world streaming */
#include <jce/renderer/jce_scene_renderer.h>   /* set_anim_sm_active */
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_thread.h>            /* async chunk-load thread pool */
#include <jce/os/core/jce_alloc.h>
}

#include "core/jce_assetdb.h"
#include "core/jce_project_settings.h"
#include "core/jce_editor_config.h"   /* jce_editor_dotjce_path (~/.jce) */
#include "scene/jce_editor_game_render.h"  /* hand Play runtime to UI dispatch */

#include <string>
#include <cstdlib>   /* getenv/atof for streaming bench toggles (M2) */

/* ── Play mode: runtime-driven ───────────────────────────────────── */

#include <jce/application/jce_runtime.h>

static EditorHistorySnapshot s_play_snapshot;
static bool                  s_play_snapshot_valid = false;

static JceRuntime *s_play_runtime = NULL;
static JceAudio   *s_play_audio   = NULL;   /* owned alongside the runtime */

/* World streaming for editor Play.  The runtime itself never ticks a streamer
 * (engine gap — jce_runtime_step has no streaming), so the Play harness owns
 * one for the session, mirroring jce_default_init_world_streaming in the
 * shipped game: built from the scene's authored streaming block, driven by the
 * live player position, torn down on Stop.  Inert unless the scene has a
 * streaming block with enabled + chunks, so non-streaming scenes are
 * byte-identical to before. */
static JceWorldStreamer *s_play_streamer        = NULL;
static JceFileSystem    *s_play_stream_fs       = NULL;
/* Background worker pool for async chunk loads (off-thread disk read + private
 * buffer; the scene APPLY/spawn stays on the main thread).  Owned alongside the
 * streamer for the Play session: destroyed AFTER the streamer (whose destroy
 * joins all in-flight chunk tasks first) so no worker can touch a freed fs. */
static JceThreadPool    *s_play_stream_pool      = NULL;
static jce_vec3          s_play_stream_pos       = { 0.0f, 0.0f, 0.0f };
static bool              s_play_stream_pos_valid = false;

/* Editor-only audio resolver: probe a few host-FS roots (VFS bundle path,
 * project root, scene-dir, and a few parents) before giving up.  Matches
 * the legacy editor Play behaviour so showcase scenes that put music
 * next to assets/ still load when opened outside a project context.    */
extern "C" {
#include <jce/os/core/jce_filesystem.h>
}

/* Resolve a scene-stored (project-relative) model/collider path to a readable
 * host path — the SAME resolution the scene renderer uses for the visual mesh
 * (ed_load_model_cb -> jce_editor_scene_asset_cache_resolve_mesh_path), so a
 * model-based collider (Compound / Mesh) resolves to the exact file its mesh
 * renders from.  Without this the runtime read the raw relative path against
 * the process CWD (the editor never chdir's) and model colliders silently
 * failed to spawn while the mesh still rendered.  Falls back to the generic
 * asset-path resolver. */
static bool editor_play_resolve_path(void * /*ud*/, const char *in,
                                     char *out, int out_size)
{
    if (!in || !in[0] || !out || out_size <= 0) return false;
    if (jce_editor_scene_asset_cache_resolve_mesh_path(in, out, out_size))
        return true;
    return jce_editor_resolve_asset_path(in, out, out_size);
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

/* Renderer anim-event hook → runtime script dispatch (P1 anim-events), the
 * editor Play twin of the shipped default_main wiring.  The editor's scene
 * renderer (jce_editor_get_scene_renderer) advances the per-clip animation
 * event tracks each frame; while Play is active we point its hook here so each
 * fired event reaches the firing entity's on_anim_event script handler through
 * the play runtime.  `user` is the JceRuntime*. */
static void editor_anim_event_cb(uint64_t entity, const JceAnimEvent *ev,
                                 void *user)
{
    jce_runtime_dispatch_anim_event((JceRuntime *)user, entity, ev);
}

/* Renderer anim-state hook → runtime script dispatch (state-enter/exit), the
 * editor Play twin of the shipped default_main wiring.  While Play is active we
 * point the renderer's anim-state hook here so each SM active-state change
 * reaches the entity's on_state_exit / on_state_enter handlers through the play
 * runtime.  `user` is the JceRuntime*. */
static void editor_anim_state_cb(uint64_t entity, const char *from_state,
                                 const char *to_state, void *user)
{
    jce_runtime_dispatch_anim_state((JceRuntime *)user, entity,
                                    from_state, to_state);
}

/* Foot IK: renderer ground-query hook -> play runtime physics raycast, so
 * FootIk components adapt feet to terrain in editor Play (cleared on Stop). */
static bool editor_ground_query_cb(uint64_t entity, const float origin[3],
                                   const float dir[3], float max_dist,
                                   float *out_hit_y, float out_normal[3],
                                   void *user)
{
    (void)entity;
    return jce_runtime_ground_raycast((JceRuntime *)user, origin, dir, max_dist,
                                      out_hit_y, out_normal);
}

/* ── Play mode API ───────────────────────────────────────────────── */

/* World-streamer entity callbacks for editor Play (streaming M3).  A streamed
 * chunk's entities must (a) appear in the editor hierarchy/selection mirror AND
 * (b) be wired into the live Play runtime so a script / trigger / NPC authored
 * in the cell actually runs (on_start / on_update, trigger observer, body) — the
 * plain attach helper only does (a).  These combined callbacks run BOTH; `user`
 * is the Play runtime.  Spawn: wire runtime gameplay first, then mirror.  Despawn
 * (fired BEFORE the streamer destroys the entities): release runtime gameplay
 * first (so no body/script/trigger dangles), then mirror-remove. */
static void play_streamer_spawn_cb(const uint64_t *ids, uint32_t count, void *user)
{
    JceRuntime *rt = (JceRuntime *)user;
    if (rt) jce_runtime_spawn_gameplay_for_ids(rt, ids, count);
    jce_state_streamer_mirror_spawn(ids, count);
}

static void play_streamer_despawn_cb(const uint64_t *ids, uint32_t count, void *user)
{
    JceRuntime *rt = (JceRuntime *)user;
    if (rt) jce_runtime_despawn_gameplay_for_ids(rt, ids, count);
    jce_state_streamer_mirror_despawn(ids, count);
}

/* ── World streaming (editor Play) ───────────────────────────────────
 * Mirrors jce_default_init_world_streaming (jce_default_main.inc.h) so the
 * editor Play button streams chunks exactly like the shipped game. */
static void play_streaming_begin(void)
{
    s_play_streamer = NULL;
    s_play_stream_fs = NULL;
    s_play_stream_pool = NULL;
    s_play_stream_pos_valid = false;
    if (!s.scene) return;

    const JceSceneStreamingSettings *st = jce_scene_get_streaming_settings(s.scene);
    if (!st || !st->enabled || st->chunk_count == 0) return;

    /* Avoid a double streamer if the scene-view Preview toggle left one live. */
    jce_editor_scene_render_streaming_teardown();

    /* Mount the source ASSET ROOT so chunk fragment paths resolve project-
     * relative — SHARED with the scene-view preview streamer via
     * jce_editor_streaming_fs_base() so both viewports stream identical content
     * (no scene-view vs game-view divergence). */
    char base[1024] = { 0 };
    if (!jce_editor_streaming_fs_base(base, sizeof base)) {
        LOG_WARN(LOG_TAG, "world streaming: no asset root resolved — disabled");
        return;
    }

    JceFileSystem *fs = jce_fs_create();
    if (!fs) return;
    jce_fs_mount_dir(fs, "", base);

    JceWorldStreamConfig wsc = jce_world_stream_config_default();
    wsc.mode            = (st->mode == 1) ? JCE_STREAM_RECTANGULAR : JCE_STREAM_RADIAL;
    wsc.load_radius     = st->load_radius;
    wsc.unload_radius   = st->unload_radius;
    wsc.max_pending     = st->max_pending;
    wsc.budget_mb       = st->budget_mb;
    wsc.frame_budget_ms = st->frame_budget_ms;

    /* Hand the streamer a small worker pool so chunk disk-read + JSON-byte
     * staging run OFF the main thread (the apply/spawn stays time-sliced on
     * main); this kills the per-cell frame hitch.  Web has no real threads, so
     * keep the cooperative single-thread path there. */
    JceThreadPool *pool = NULL;
#if !JCE_PLATFORM_WEB
    /* Bench/diagnostic toggle (M2 A/B): JCE_STREAM_SYNC=1 forces the synchronous
     * single-thread chunk-load path (no worker pool) so the async chunk-load
     * benefit can be measured, and as a safety hatch if the pool ever misbehaves.
     * Mirrors JCE_DISABLE_WCACHE for the render-cache A/B. */
    const char *stream_sync = getenv("JCE_STREAM_SYNC");
    const bool force_sync = (stream_sync && stream_sync[0] && stream_sync[0] != '0');
    if (!force_sync)
        pool = jce_thread_pool_create(3);
    else
        LOG_INFO(LOG_TAG, "JCE_STREAM_SYNC=1: forcing synchronous chunk loads (no pool)");
#endif
    wsc.single_thread   = (pool == NULL);  /* async iff we have a pool */

    s_play_streamer = jce_world_streamer_create(&wsc, s.scene, fs, pool);
    if (!s_play_streamer) {
        if (pool) jce_thread_pool_destroy(pool);
        jce_fs_destroy(fs);
        LOG_WARN(LOG_TAG, "play world streamer creation failed — streaming disabled");
        return;
    }
    s_play_stream_pool = pool;
    s_play_stream_fs = fs;
    jce_world_streamer_register_from_scene_settings(s_play_streamer, st);
    /* Mirror Play-streamed entities into the editor hierarchy/selection AND wire
     * them into the live Play runtime gameplay (streaming M3) — a script /
     * trigger / NPC authored in a streamed cell must actually run, not just
     * render.  These combined callbacks do both; the plain attach helper (used by
     * the scene-view preview, which has no runtime) does only the mirror. */
    jce_world_streamer_set_entity_callbacks(s_play_streamer,
                                            play_streamer_spawn_cb,
                                            play_streamer_despawn_cb,
                                            s_play_runtime);
    /* Toggle the always-resident HLOD far-skyline proxies as chunks (un)load. */
    jce_state_attach_streamer_hlod(s_play_streamer);
    LOG_INFO(LOG_TAG,
             "editor Play world streaming active (%u chunks, r=%.0f/%.0f, root=%s)",
             jce_world_streamer_chunk_count(s_play_streamer),
             wsc.load_radius, wsc.unload_radius, base);
}

static void play_streaming_tick(void)
{
    if (!s_play_streamer || !s_play_runtime) return;
    jce_vec3 pos;
    if (jce_runtime_get_player_position(s_play_runtime, &pos)) {
        s_play_stream_pos = pos;
        s_play_stream_pos_valid = true;
    }
    /* KPI bench driver (M2 traversal): JCE_KPI_TRAVERSE=1 advances the streaming
     * center deterministically along +X so cells continuously load/unload while
     * the autoplay player stays stationary — the only way to exercise the
     * streamer's load/unload churn under the headless KPI harness. Overrides the
     * streamer center directly (the runtime player never moves under autoplay).
     *   JCE_KPI_TRAVERSE_STEP   world units advanced per tick (default 2.0)
     *   JCE_KPI_TRAVERSE_START  starting X (default 0.0; e.g. -2700 to sweep
     *                           from the world's -X edge through the populated band) */
    static int s_traverse = -1;
    if (s_traverse < 0) {
        const char *tv = getenv("JCE_KPI_TRAVERSE");
        s_traverse = (tv && tv[0] && tv[0] != '0') ? 1 : 0;
    }
    if (s_traverse) {
        static int s_trav_init = 0;
        static float s_trav_x = 0.0f;
        const char *step_env  = getenv("JCE_KPI_TRAVERSE_STEP");
        const char *start_env = getenv("JCE_KPI_TRAVERSE_START");
        const float step = (step_env && step_env[0]) ? (float)atof(step_env) : 2.0f;
        if (!s_trav_init) {
            s_trav_x = (start_env && start_env[0]) ? (float)atof(start_env) : 0.0f;
            s_trav_init = 1;
        }
        s_trav_x += step;             /* +X each tick → continuous load/unload churn */
        s_play_stream_pos.x = s_trav_x;
        s_play_stream_pos_valid = true;
    }
    /* Until a CharacterController exists, hold last-known (or origin) so the
     * inner ring around spawn still streams in on frame 0. */
    jce_world_streamer_update(s_play_streamer, s_play_stream_pos);
}

static void play_streaming_end(void)
{
    /* Order matters: jce_world_streamer_destroy → jce_streaming_destroy joins
     * (jce_task_wait) every in-flight chunk task before freeing, so once the
     * streamer is gone no worker is still touching the fs/scene.  Only then is
     * it safe to destroy the pool (which also waits-for-all on shutdown) and
     * the fs the workers were reading from. */
    if (s_play_streamer)   { jce_world_streamer_destroy(s_play_streamer); s_play_streamer = NULL; }
    if (s_play_stream_pool){ jce_thread_pool_destroy(s_play_stream_pool); s_play_stream_pool = NULL; }
    if (s_play_stream_fs)  { jce_fs_destroy(s_play_stream_fs);            s_play_stream_fs = NULL; }
    s_play_stream_pos_valid = false;
    /* Re-show all HLOD proxies so the master skyline is whole again after Play. */
    jce_state_detach_streamer_hlod();
}

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
    rd.resolve_path_fn = editor_play_resolve_path;
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

    /* Phase 0.2: seed the runtime's global time scale from the project's Time
     * settings (Unity-style default timeScale — previously built-but-unwired).
     * Scripts (jce.set_time_scale / jce.pause) override it live during play. */
    if (ps && ps->time.time_scale > 0.0f)
        jce_runtime_set_time_scale(s_play_runtime, ps->time.time_scale);

    jce_editor_scene_reset_anim_timer();
    /* Let bound animation state machines own active_clip while playing (in the
     * editor they stay idle so manual clip preview keeps working). */
    jce_scene_renderer_set_anim_sm_active(jce_editor_get_scene_renderer(), true);
    /* Route animation frame events into the play runtime's script VM so
     * authored on_anim_event handlers fire in editor Play just like a shipped
     * game (cleared on Stop below). */
    jce_scene_renderer_set_anim_event_fn(jce_editor_get_scene_renderer(),
                                         editor_anim_event_cb, s_play_runtime);
    /* Route SM state changes into the play runtime's script VM so authored
     * on_state_enter / on_state_exit handlers fire in editor Play just like a
     * shipped game (cleared on Stop below). */
    jce_scene_renderer_set_anim_state_fn(jce_editor_get_scene_renderer(),
                                         editor_anim_state_cb, s_play_runtime);
    /* Foot IK ground-query hook (cleared on Stop below, before runtime destroy). */
    jce_scene_renderer_set_ground_query_fn(jce_editor_get_scene_renderer(),
                                           editor_ground_query_cb, s_play_runtime);
    /* Hand the game renderer the live runtime so canvas UI events (button
     * clicks, slider/toggle/dropdown value changes, input-field edits + submit)
     * dispatch to the gameplay script VM in editor Play, exactly like a shipped
     * game (detached on Stop below). */
    jce_editor_game_render_set_play_runtime(s_play_runtime);
    /* World streaming for this Play session (inert unless the scene authored a
     * streaming block) — the runtime has no streamer of its own. */
    play_streaming_begin();
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

    /* Clear the renderer's anim-event hook BEFORE destroying the runtime it
     * forwards to — otherwise a stray render frame could route an event into a
     * freed runtime pointer. */
    jce_scene_renderer_set_anim_event_fn(jce_editor_get_scene_renderer(),
                                         NULL, NULL);
    jce_scene_renderer_set_anim_state_fn(jce_editor_get_scene_renderer(),
                                         NULL, NULL);
    jce_scene_renderer_set_ground_query_fn(jce_editor_get_scene_renderer(),
                                           NULL, NULL);
    /* Detach the runtime from the game renderer's UI dispatch BEFORE destroying
     * it, so a stray render frame can't route a drained click into a freed
     * runtime pointer (mirrors the renderer-hook clears above). */
    jce_editor_game_render_set_play_runtime(NULL);

    /* Unload streamed chunks (destroys their entities from the live scene)
     * BEFORE the runtime is destroyed and BEFORE the pre-play snapshot is
     * restored below, so streamed content never lingers or bakes in. */
    play_streaming_end();

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
    /* Restore the scene-view streaming preview (no-op unless the World Streaming
     * → Preview toggle is on) — play_streaming_begin tore it down so only ONE
     * streamer spawns into the shared scene during Play.  Both use the same FS
     * base (jce_editor_streaming_fs_base) so content matches across viewports. */
    jce_editor_scene_render_streaming_rebuild();
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
    if (s_play_runtime) {
        jce_runtime_step(s_play_runtime, dt > 0 ? dt : 1.0f/60.0f);
        jce_state_prune_dead();   /* sync mirror list with runtime jce.destroy */
        play_streaming_tick();    /* load/unload chunks around the player */
    }
}

void jce_state_play_mode_tick(float dt)
{
    if (s.play_state != JCE_PLAY_PLAYING) return;
    if (s_play_runtime) {
        jce_runtime_step(s_play_runtime, dt);
        jce_state_prune_dead();   /* drop runtime-destroyed entities (jce.destroy)
                                   * so the hierarchy never touches a dead handle */
        play_streaming_tick();    /* load/unload chunks around the player */
    }
}

/* ── External hooks (game view / scene render) ──────────────────── */

void jce_editor_play_set_player_input(float walk_x, float walk_z,
                                      bool jump_pressed, bool jump_held,
                                      bool sprint, bool attack)
{
    if (!s_play_runtime) return;
    JceRuntimeInput in = {};
    in.walk_x         = walk_x;
    in.walk_z         = walk_z;
    in.jump_pressed   = jump_pressed;
    in.jump_held      = jump_held;
    in.sprint         = sprint;
    in.attack_pressed = attack;
    in.speed_mult     = 1.0f;   /* sprint scaling is authored on the component */
    jce_runtime_set_input(s_play_runtime, &in);
}

/* Top 6: bind the live editor action map so editor-Play scripts can query
 * authored actions by name (jce.is_action_down / get_axis), like a shipped
 * game.  The runtime borrows the pointer for this frame. */
void jce_editor_play_set_actions(const JceInputActions *actions)
{
    if (!s_play_runtime) return;
    jce_runtime_set_actions(s_play_runtime, actions);
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

bool jce_editor_play_get_player_forward(float *out_x, float *out_y, float *out_z)
{
    if (!s_play_runtime) return false;
    jce_vec3 f;
    if (!jce_runtime_get_player_forward(s_play_runtime, &f)) return false;
    if (out_x) *out_x = f.x;
    if (out_y) *out_y = f.y;
    if (out_z) *out_z = f.z;
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
    /* Source entity's prefab path (captured at copy time so Paste As Instance
     * works even after the source is deleted). "" = not a prefab instance. */
    char        prefab_path[256];
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
    const char *pp = jce_state_entity_prefab_path(id);
    snprintf(out->prefab_path, sizeof(out->prefab_path), "%s", pp ? pp : "");
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

/* Paste As Instance: for each clipboard entry whose source was a prefab
 * instance, instantiate a fresh LINKED instance of that prefab (so it tracks
 * the prefab asset); non-prefab entries fall back to a normal subtree paste.
 * Never consumes a cut clipboard (instancing is a copy-like op). */
int jce_state_paste_entities_as_instance(uint32_t parent_id,
                                         uint32_t *out_ids, int max_out)
{
    if (s_clip_entries.empty()) return 0;
    int n = 0;
    jce_state_begin_batch_edit();
    for (const auto &e : s_clip_entries) {
        uint32_t new_id = (e.prefab_path[0] != '\0')
            ? jce_state_instantiate_prefab(e.prefab_path, parent_id)
            : clip_paste_one(e, parent_id, " (Paste)");
        if (new_id == 0) continue;
        if (out_ids && n < max_out) out_ids[n] = new_id;
        ++n;
    }
    jce_state_end_batch_edit();
    LOG_INFO(LOG_TAG, "pasted %d entities as instances under %u", n, parent_id);
    return n;
}
