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
