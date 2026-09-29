/*
 * jce_save_providers.c — Built-in snapshot providers.
 *
 * Wires concrete engine subsystems into the generic jce_snapshot registry.
 * Currently provides the scene/ECS provider; gameplay providers (trigger /
 * spawn / weapon live state) plug in here as they gain stable schemas.
 */
#include <jce/middleware/save/jce_save_providers.h>
#include <jce/middleware/save/jce_save_migration.h>

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/middleware/world/jce_environment.h>

#include <math.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#define LOG_TAG "save"

/* ================================================================== */
/* scene_ecs provider                                                  */
/* ================================================================== */

#define SCENE_PROVIDER_ID       "scene_ecs"
#define SCENE_PROVIDER_VERSION  1u

/*
 * Per-provider context.  The snapshot read callback only receives one void*
 * user pointer, so bundle the scene with the (optional) migration registry
 * the read path consults to upgrade older saves.  Owned by the registration
 * and freed via the unregister helper.
 */
typedef struct {
    JceScene                 *scene;
    JceSaveMigrationRegistry *migrations;   /* may be NULL */
} SceneProviderCtx;

/*
 * Capture the full scene to a JSON document and write it into the section
 * as a length-prefixed string.  The component-JSON schema is shared with
 * the editor's scene serializer, so a save round-trips every authored
 * component the engine understands.
 */
static bool scene_write(JceSnapshotStream *s, void *user)
{
    SceneProviderCtx *ctx = (SceneProviderCtx *)user;
    JceScene *scene = ctx ? ctx->scene : NULL;
    if (!scene) return false;

    JceJson *root = jce_scene_save_json(scene);
    if (!root) {
        LOG_ERROR(LOG_TAG, " scene_ecs: serialize returned NULL");
        return false;
    }

    char *text = jce_json_print(root, false);
    jce_json_free(root);
    if (!text) {
        LOG_ERROR(LOG_TAG, " scene_ecs: json print failed");
        return false;
    }

    bool ok = jce_snap_write_string(s, text);
    jce_json_free_string(text);
    return ok;
}

/*
 * Clear the scene and rehydrate every entity from the section's JSON
 * payload.  Saves written at an older section version are MIGRATED up to the
 * current schema through the migration registry (when one is wired) before
 * the scene loader consumes them; future versions, and older versions with
 * no migration path, are refused rather than silently mis-parsed.
 */
static bool scene_read(JceSnapshotStream *s, uint32_t loaded_version, void *user)
{
    SceneProviderCtx *ctx = (SceneProviderCtx *)user;
    JceScene *scene = ctx ? ctx->scene : NULL;
    if (!scene) return false;
    if (loaded_version > SCENE_PROVIDER_VERSION) {
        LOG_ERROR(LOG_TAG, " scene_ecs: unsupported section version %u",
                  loaded_version);
        return false;
    }

    char *text = NULL;
    if (!jce_snap_read_string(s, &text) || !text) return false;

    JceJson *root = jce_json_parse(text, 0);
    JCE_FREE(text);
    if (!root) {
        LOG_ERROR(LOG_TAG, " scene_ecs: json parse failed");
        return false;
    }

    /* Upgrade an older save to the current schema.  This is a strict no-op
     * when loaded_version == SCENE_PROVIDER_VERSION (the common case): zero
     * lookups, JSON untouched.  A missing step on the path is reported by
     * jce_save_migrate() and aborts the load. */
    if (loaded_version != SCENE_PROVIDER_VERSION) {
        if (!jce_save_migrate(ctx->migrations, SCENE_PROVIDER_ID,
                              loaded_version, SCENE_PROVIDER_VERSION, root)) {
            LOG_ERROR(LOG_TAG,
                      " scene_ecs: could not migrate save v%u to v%u",
                      loaded_version, SCENE_PROVIDER_VERSION);
            jce_json_free(root);
            return false;
        }
        LOG_INFO(LOG_TAG, " scene_ecs: migrated save v%u -> v%u",
                 loaded_version, SCENE_PROVIDER_VERSION);
    }

    jce_scene_clear(scene);
    int loaded = jce_scene_load_json(scene, root);
    jce_json_free(root);
    if (loaded < 0) {
        LOG_ERROR(LOG_TAG, " scene_ecs: load_json failed");
        return false;
    }
    LOG_INFO(LOG_TAG, " scene_ecs: restored %d entities", loaded);
    return true;
}

bool jce_save_register_scene_provider_ex(JceSnapshotRegistry      *reg,
                                         JceScene                 *scene,
                                         JceSaveMigrationRegistry *migrations)
{
    if (!reg || !scene) return false;

    /* Reuse the heap context from a prior registration (the runtime
     * re-registers on every scene reload to re-point at the live scene) so
     * we update in place rather than leaking the old one. */
    SceneProviderCtx *ctx =
        (SceneProviderCtx *)jce_snapshot_get_user(reg, SCENE_PROVIDER_ID);
    if (!ctx) {
        ctx = (SceneProviderCtx *)JCE_MALLOC(sizeof(*ctx));
        if (!ctx) return false;
    }
    ctx->scene      = scene;
    ctx->migrations = migrations;

    jce_snapshot_register(reg, SCENE_PROVIDER_ID, SCENE_PROVIDER_VERSION,
                          scene_write, scene_read, ctx);
    return true;
}

bool jce_save_register_scene_provider(JceSnapshotRegistry *reg, JceScene *scene)
{
    return jce_save_register_scene_provider_ex(reg, scene, NULL);
}

void jce_save_unregister_scene_provider(JceSnapshotRegistry *reg)
{
    if (!reg) return;
    SceneProviderCtx *ctx =
        (SceneProviderCtx *)jce_snapshot_get_user(reg, SCENE_PROVIDER_ID);
    jce_snapshot_unregister(reg, SCENE_PROVIDER_ID);
    JCE_FREE(ctx);
}

/* ── Scene environment ("scene_env" v1) ────────────────────────────────
 *
 * Four ACCUMULATED values, and deliberately only four.  A session earns the
 * hour, the monotonic clock behind it, and the two surface integrators; it
 * cannot re-derive them, because they are the integral of everything that
 * happened.  The rest of JceEnvironmentState -- weather, wind, humidity,
 * temperature, and the sun placed from the hour -- is recomputed from the
 * authored settings on the very next jce_scene_environment_advance(), so
 * writing it here would be persisting a cache and then arguing with it.
 *
 * Not in the scene_ecs section, and not in the scene JSON schema, on purpose:
 * a running clock in the authored scene file means saving a level from the
 * editor bakes whatever hour the preview had reached into it.  That is the
 * exact failure the environment authority move was made to end.
 */

#define ENV_PROVIDER_ID "scene_env"

static bool env_provider_write(JceSnapshotStream *s, void *user)
{
    JceScene *scene = (JceScene *)user;
    if (!s || !scene) return false;

    const JceEnvironmentState *env = jce_scene_environment(scene);
    if (!env) return false;

    return jce_snap_write_f32(s, jce_scene_environment_hour(scene))
        && jce_snap_write_u64(s, (uint64_t)(env->world_time_seconds * 1000.0))
        && jce_snap_write_f32(s, env->global_wetness)
        && jce_snap_write_f32(s, env->snow_amount);
}

static bool env_provider_read(JceSnapshotStream *s, uint32_t loaded_version,
                              void *user)
{
    JceScene *scene = (JceScene *)user;
    if (!s || !scene) return false;
    /* v1 is the only shape.  A FUTURE version is refused rather than guessed
     * at: silently reading four floats out of a longer record would restore a
     * plausible wrong world. */
    if (loaded_version != 1u) return false;

    float    hour = 0.0f, wetness = 0.0f, snow = 0.0f;
    uint64_t world_ms = 0u;
    if (!jce_snap_read_f32(s, &hour)) return false;
    if (!jce_snap_read_u64(s, &world_ms)) return false;
    if (!jce_snap_read_f32(s, &wetness)) return false;
    if (!jce_snap_read_f32(s, &snow)) return false;

    JceEnvironmentState *env = jce_scene_environment(scene);
    if (!env) return false;

    /* Hour first: set_hour also rewrites world_time_seconds from the fraction,
     * so restoring the saved monotonic clock has to come after it or it would
     * be overwritten by a value derived from the hour alone. */
    jce_scene_environment_set_hour(scene, hour);
    env->world_time_seconds = (double)world_ms / 1000.0;
    env->global_wetness     = wetness;
    env->snow_amount        = snow;
    /* Mark the AUTHORED seed as already consumed.
     *
     * day_seed_hour records "the authored value we last applied", not the live
     * hour -- the advance re-seeds when the authored hour differs from it, and
     * that is how a designer moving the slider takes effect.  Storing the
     * RESTORED hour here therefore does the opposite of what it looks like: on
     * the next frame the authored 08:00 differs from the restored 20:00, the
     * advance reads that as an edit, and the loaded save snaps back to the
     * start of the level.  Measured exactly that way, on the first run of this
     * provider's own test: expected 21.00, got 9.00.
     *
     * Wrapped the same way jce_scene_environment_advance wraps it, so an
     * authored hour outside [0,24) compares equal rather than re-seeding every
     * frame forever. */
    const JceSceneRenderingSettings *rs = jce_scene_get_rendering_settings(scene);
    float seed = rs ? fmodf(rs->tod_hour, 24.0f) : 0.0f;
    if (seed < 0.0f) seed += 24.0f;
    if (!(seed == seed)) seed = 0.0f;                    /* NaN */
    env->day_seed_hour = seed;
    return true;
}

bool jce_save_register_env_provider(JceSnapshotRegistry *reg, JceScene *scene)
{
    if (!reg || !scene) return false;
    jce_snapshot_register(reg, ENV_PROVIDER_ID, 1u,
                          env_provider_write, env_provider_read, scene);
    return true;
}
