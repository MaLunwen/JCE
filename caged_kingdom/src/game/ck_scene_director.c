/*
 * ck_scene_director.c  Implementation of the runtime scene swapper.
 *
 * Workflow (v1):
 *   create()          -> alloc director, create JceScene + JceSceneRenderer
 *   load_initial(p)   -> ck_player_state_init() + load(p)
 *   load(p)           -> jce_scene_clear() + serial_load_vfs(p)
 *   destroy()         -> tear scene/renderer down
 *
 * Engine API used:
 *   jce_scene_create / jce_scene_destroy / jce_scene_clear
 *   jce_scene_renderer_create / jce_scene_renderer_destroy
 *   jce_fs_create / jce_fs_mount_pak / jce_fs_destroy
 *   jce_scene_serial_load_vfs
 *
 * No game-level concepts leak out: triggers, fades, and quest graph
 * sit in their own modules (see SCENES_DESIGN.md appendix A).
 */

#include "ck_scene_director.h"

#include "ck_quest_graph.h"
#include "ck_trigger.h"

#include <jce/api.h>

#include <string.h>

#define LOG_TAG       "ck_director"
#define QUEST_GRAPH_PATH "quests/main_storyline.json"

/* Cooldown after a transition fires, in seconds, to avoid the next
   scene's spawn point firing its own zone on frame 1. */
#define POST_TRANSITION_LOCK_SEC 0.50f

struct CkSceneDirector {
    JceRenderer       *renderer;
    JcePakArchive     *pak;
    JceAudio          *audio;

    /* Persistent VFS handle: mounted once with the game PAK, reused for
       every scene/quest/trigger load.  This avoids the per-load mount
       overhead and the small leak risk in error paths. */
    JceFileSystem     *fs;

    JceScene          *scene;
    JceSceneRenderer  *scene_renderer;
    JceRuntime        *runtime;

    /* Game-data systems owned here so transitions stay atomic: clearing
       the scene and refreshing the trigger set happen back-to-back, and
       the quest graph is the source of truth for "what comes next". */
    CkQuestGraph      *quests;
    CkTriggerSet      *triggers;

    CkPlayerState      player;

    /* Suppresses trigger checks for a short window after a load(), so
       the next scene's spawn point won't immediately fire its zones. */
    float              post_transition_lock;

    /* Path of the most recently loaded scene.  Bounded so we never have
       to allocate just to remember the name; longer paths are truncated. */
    char               current_path[256];
};

/* ── Helpers ───────────────────────────────────────────────────────── */

static void set_current_path(CkSceneDirector *dir, const char *vfs_path)
{
    size_t n = strlen(vfs_path);
    if (n >= sizeof(dir->current_path)) n = sizeof(dir->current_path) - 1;
    memcpy(dir->current_path, vfs_path, n);
    dir->current_path[n] = '\0';
}

static void set_active_quest(CkPlayerState *p, const char *qid)
{
    if (!p || !qid) return;
    size_t n = strlen(qid);
    if (n >= sizeof(p->active_quest)) n = sizeof(p->active_quest) - 1;
    memcpy(p->active_quest, qid, n);
    p->active_quest[n] = '\0';
}

/* Reload the trigger set for the freshly loaded scene + sync the
   player's active_quest tag from the scene's `ck_quest.questId`.
   Tolerant of scenes without a `ck_quest` block (just clears triggers). */
static void refresh_quest_state(CkSceneDirector *dir, const char *vfs_path)
{
    ck_trigger_set_destroy(dir->triggers);
    dir->triggers = ck_trigger_set_load_vfs(dir->fs, vfs_path);

    const char *qid = ck_trigger_set_quest_id(dir->triggers);
    if (qid && qid[0]) {
        set_active_quest(&dir->player, qid);
        LOG_INFO(LOG_TAG, "active quest -> '%s' (%zu trigger zone(s))",
                 qid, ck_trigger_set_count(dir->triggers));
    } else {
        LOG_INFO(LOG_TAG, "scene '%s' has no ck_quest block (zones=0)",
                 vfs_path);
    }
}

/* ── Hot-reload glue (dev mode only) ───────────────────────────────── */

typedef struct {
    JceScene *scene;
    const char *changed_path;
    const JcePbrMaterial *pbr;
    const char (*tex_paths)[256];   /* 5 slots */
    int hit_count;
} PatchCtx;

static void patch_matching_mesh_renderer(JceScene *s, JceEntity e, void *ud)
{
    PatchCtx *ctx = (PatchCtx *)ud;
    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(s, e);
    if (!mr) return;
    if (mr->material_path[0] == '\0') return;
    if (strcmp(mr->material_path, ctx->changed_path) != 0) return;

    /* Apply new PBR factors. We deliberately keep mesh_path,
     * material_path, and per-entity overrides like base_color
     * alpha untouched only when the new file specifies them — the
     * .mat.json is authoritative for everything in the PBR block. */
    const JcePbrMaterial *p = ctx->pbr;
    mr->base_color[0] = p->base_color_factor[0];
    mr->base_color[1] = p->base_color_factor[1];
    mr->base_color[2] = p->base_color_factor[2];
    mr->base_color[3] = p->base_color_factor[3];
    mr->metallic     = p->metallic_factor;
    mr->roughness    = p->roughness_factor;
    mr->emissive[0]  = p->emissive_factor[0];
    mr->emissive[1]  = p->emissive_factor[1];
    mr->emissive[2]  = p->emissive_factor[2];
    mr->normal_scale = p->normal_scale;
    mr->ao_strength  = p->ao_strength;
    mr->alpha_mode   = (uint8_t)p->alpha_mode;
    mr->alpha_cutoff = p->alpha_cutoff;
    mr->double_sided = p->double_sided;

    /* Texture slots: only overwrite when the new file actually names
     * a texture, so a designer can save a factor tweak without losing
     * a per-entity texture override. */
    const char (*tp)[256] = ctx->tex_paths;
    if (tp[0][0]) mr->albedo_tex   = jce_scene_intern(s, tp[0]);
    if (tp[1][0]) mr->mr_tex       = jce_scene_intern(s, tp[1]);
    if (tp[2][0]) mr->normal_tex   = jce_scene_intern(s, tp[2]);
    if (tp[3][0]) mr->ao_tex       = jce_scene_intern(s, tp[3]);
    if (tp[4][0]) mr->emissive_tex = jce_scene_intern(s, tp[4]);

    ctx->hit_count++;
}

static void on_material_reloaded(const char *vfs_path,
                                 const JcePbrMaterial *pbr,
                                 const char tex_paths[5][256],
                                 void *user)
{
    CkSceneDirector *dir = (CkSceneDirector *)user;
    if (!dir || !dir->scene) return;
    PatchCtx ctx;
    ctx.scene        = dir->scene;
    ctx.changed_path = vfs_path;
    ctx.pbr          = pbr;
    ctx.tex_paths    = tex_paths;
    ctx.hit_count    = 0;
    jce_scene_each_entity(dir->scene, patch_matching_mesh_renderer, &ctx);
    LOG_INFO(LOG_TAG, "applied '%s' to %d entity(ies)",
             vfs_path, ctx.hit_count);
}

static void track_scene_materials(JceScene *s, JceEntity e, void *ud)
{
    (void)ud;
    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(s, e);
    if (mr && mr->material_path[0]) {
        jce_material_registry_track(mr->material_path);
    }
}

static bool dir_load_path(CkSceneDirector *dir, const char *vfs_path)
{
    if (!dir || !dir->scene || !vfs_path || !vfs_path[0]) return false;

    bool ok = jce_scene_serial_load_vfs(dir->scene, dir->fs, vfs_path);
    if (!ok) {
        LOG_ERROR(LOG_TAG, "scene load failed: %s", vfs_path);
        dir->current_path[0] = '\0';
        ck_trigger_set_destroy(dir->triggers);
        dir->triggers = NULL;
        return false;
    }

    {
        JceRuntimeDesc runtime_desc = {0};
        runtime_desc.scene = dir->scene;
        runtime_desc.pak = dir->pak;
        runtime_desc.audio = dir->audio;
        runtime_desc.enable_physics = true;
        dir->runtime = jce_runtime_create(&runtime_desc);
        if (!dir->runtime) {
            LOG_ERROR(LOG_TAG, "runtime create failed for scene: %s", vfs_path);
            jce_scene_clear(dir->scene);
            dir->current_path[0] = '\0';
            ck_trigger_set_destroy(dir->triggers);
            dir->triggers = NULL;
            return false;
        }
    }

    set_current_path(dir, vfs_path);
    refresh_quest_state(dir, vfs_path);
    dir->post_transition_lock = POST_TRANSITION_LOCK_SEC;

    /* Dev-mode: rebuild the hot-reload watch list for this scene. */
    jce_material_registry_clear();
    jce_scene_each_entity(dir->scene, track_scene_materials, NULL);
    jce_vcam_system_reset();

    LOG_INFO(LOG_TAG, "scene loaded: %s", vfs_path);
    return true;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

CkSceneDirector *ck_scene_director_create(JceRenderer *renderer,
                                          JcePakArchive *pak,
                                          JceAudio *audio)
{
    if (!renderer || !pak) {
        LOG_ERROR(LOG_TAG, "create: renderer/pak must be non-NULL");
        return NULL;
    }

    CkSceneDirector *dir = (CkSceneDirector *)jce_malloc(sizeof(*dir));
    if (!dir) return NULL;
    memset(dir, 0, sizeof(*dir));

    dir->renderer = renderer;
    dir->pak      = pak;
    dir->audio    = audio;

    dir->fs = jce_fs_create();
    if (!dir->fs) {
        LOG_ERROR(LOG_TAG, "jce_fs_create failed");
        jce_free(dir);
        return NULL;
    }
    if (!jce_fs_mount_pak(dir->fs, pak)) {
        LOG_ERROR(LOG_TAG, "jce_fs_mount_pak failed — no asset source");
        jce_fs_destroy(dir->fs);
        jce_free(dir);
        return NULL;
    }

    /* Dev-mode override: when the editor (or a developer) passes
     * `--dev <project_assets_dir>`, mount that loose folder so any
     * .mat.json / .scene.json / texture saved by the editor is picked
     * up immediately by the running game without rebuilding the PAK.
     * jce_filesystem checks loose mounts BEFORE the PAK (see header
     * contract), so this gives source files priority. */
    {
        char dev_dir[512];
        if (jce_args_get_dev_assets(dev_dir, sizeof(dev_dir))) {
            if (jce_fs_mount_dir(dir->fs, "", dev_dir)) {
                LOG_INFO(LOG_TAG,
                    "dev-mode: loose assets mounted from '%s' (overrides PAK)",
                    dev_dir);
                jce_material_registry_init(dev_dir);
                jce_material_registry_set_reload_cb(on_material_reloaded, dir);
            } else {
                /* Hot-reload stays off rather than watching a folder the VFS
                 * never mounted: the game runs on PAK assets as usual. */
                LOG_WARN(LOG_TAG,
                    "dev-mode: could not mount '%s' — PAK assets only",
                    dev_dir);
            }
        }
    }

    dir->scene = jce_scene_create();
    if (!dir->scene) {
        jce_fs_destroy(dir->fs);
        jce_free(dir);
        return NULL;
    }

    dir->scene_renderer = jce_scene_renderer_create(renderer, pak, NULL);
    if (!dir->scene_renderer) {
        jce_scene_destroy(dir->scene);
        jce_fs_destroy(dir->fs);
        jce_free(dir);
        return NULL;
    }

    /* Storyline graph is optional: a missing file just disables
       transitions, the rest of the director still works. */
    dir->quests = ck_quest_graph_load_vfs(dir->fs, QUEST_GRAPH_PATH);
    if (!dir->quests) {
        LOG_WARN(LOG_TAG, "quest graph not loaded — transitions disabled");
    }

    ck_player_state_init(&dir->player);
    dir->current_path[0]      = '\0';
    dir->post_transition_lock = 0.0f;

    LOG_SUCCESS(LOG_TAG, "director created");
    return dir;
}

void ck_scene_director_destroy(CkSceneDirector *dir)
{
    if (!dir) return;
    jce_material_registry_shutdown();
    ck_trigger_set_destroy(dir->triggers);
    ck_quest_graph_destroy(dir->quests);
    if (dir->runtime)        jce_runtime_destroy(dir->runtime);
    if (dir->scene_renderer) jce_scene_renderer_destroy(dir->scene_renderer);
    if (dir->scene)          jce_scene_destroy(dir->scene);
    if (dir->fs)             jce_fs_destroy(dir->fs);
    jce_free(dir);
    LOG_INFO(LOG_TAG, "director destroyed");
}

/* ── Scene transport ───────────────────────────────────────────────── */

bool ck_scene_director_load_initial(CkSceneDirector *dir, const char *vfs_path)
{
    if (!dir) return false;
    ck_player_state_init(&dir->player);
    if (dir->runtime) {
        jce_runtime_destroy(dir->runtime);
        dir->runtime = NULL;
    }
    jce_scene_clear(dir->scene);
    return dir_load_path(dir, vfs_path);
}

bool ck_scene_director_load_start(CkSceneDirector *dir)
{
    if (!dir || !dir->quests) return false;
    const char *start_id    = ck_quest_graph_start(dir->quests);
    const char *start_scene = ck_quest_graph_scene(dir->quests, start_id);
    if (!start_scene) {
        LOG_ERROR(LOG_TAG, "quest graph has no start scene");
        return false;
    }
    LOG_INFO(LOG_TAG, "starting at quest '%s' -> '%s'",
             start_id, start_scene);
    return ck_scene_director_load_initial(dir, start_scene);
}

bool ck_scene_director_load(CkSceneDirector *dir, const char *vfs_path)
{
    if (!dir || !dir->scene) return false;

    if (dir->runtime) {
        jce_runtime_destroy(dir->runtime);
        dir->runtime = NULL;
    }

    int cleared = jce_scene_clear(dir->scene);
    LOG_INFO(LOG_TAG, "transition: cleared %d entities", cleared);

    return dir_load_path(dir, vfs_path);
}

/* ── Tick ──────────────────────────────────────────────────────────── */

void ck_scene_director_tick(CkSceneDirector *dir, float dt_sec,
                            const float player_pos[3])
{
    if (!dir) return;

    if (dir->runtime) jce_runtime_step(dir->runtime, dt_sec);

    /* Dev-mode .mat.json hot-reload poll. Cheap (no-op outside dev). */
    jce_material_registry_poll((double)dt_sec);

    if (dir->post_transition_lock > 0.0f) {
        dir->post_transition_lock -= dt_sec;
        if (dir->post_transition_lock < 0.0f) dir->post_transition_lock = 0.0f;
        return;
    }
    if (!dir->triggers || ck_trigger_set_count(dir->triggers) == 0) return;
    if (!player_pos) return;

    const char *fired = ck_trigger_set_check(dir->triggers, player_pos);
    if (!fired) return;

    const char *active = dir->player.active_quest;
    LOG_INFO(LOG_TAG,
             "trigger '%s' fired in quest '%s' at (%.2f, %.2f, %.2f)",
             fired,
             active[0] ? active : "<none>",
             player_pos[0], player_pos[1], player_pos[2]);

    if (!dir->quests) {
        LOG_WARN(LOG_TAG, "no quest graph loaded; trigger ignored");
        /* Disarm to avoid spamming the log every frame. */
        ck_trigger_set_destroy(dir->triggers);
        dir->triggers = NULL;
        return;
    }

    const char *next_scene = ck_quest_graph_next_scene(dir->quests, active);
    if (!next_scene) {
        LOG_INFO(LOG_TAG, "quest '%s' is terminal; staying on current scene",
                 active[0] ? active : "<none>");
        ck_trigger_set_destroy(dir->triggers);
        dir->triggers = NULL;
        return;
    }

    LOG_INFO(LOG_TAG, "transitioning to '%s'", next_scene);
    if (!ck_scene_director_load(dir, next_scene)) {
        LOG_ERROR(LOG_TAG, "transition to '%s' failed; staying put",
                  next_scene);
    }
}

/* ── Accessors ─────────────────────────────────────────────────────── */

JceScene *ck_scene_director_scene(CkSceneDirector *dir)
{
    return dir ? dir->scene : NULL;
}

JceSceneRenderer *ck_scene_director_renderer(CkSceneDirector *dir)
{
    return dir ? dir->scene_renderer : NULL;
}

CkPlayerState *ck_scene_director_player(CkSceneDirector *dir)
{
    return dir ? &dir->player : NULL;
}

const char *ck_scene_director_current_path(const CkSceneDirector *dir)
{
    return dir ? dir->current_path : "";
}
