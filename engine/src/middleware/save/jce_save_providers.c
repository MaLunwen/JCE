/*
 * jce_save_providers.c — Built-in snapshot providers.
 *
 * Wires concrete engine subsystems into the generic jce_snapshot registry.
 * Currently provides the scene/ECS provider; gameplay providers (trigger /
 * spawn / weapon live state) plug in here as they gain stable schemas.
 */
#include <jce/middleware/save/jce_save_providers.h>

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
 * Capture the full scene to a JSON document and write it into the section
 * as a length-prefixed string.  The component-JSON schema is shared with
 * the editor's scene serializer, so a save round-trips every authored
 * component the engine understands.
 */
static bool scene_write(JceSnapshotStream *s, void *user)
{
    JceScene *scene = (JceScene *)user;
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
 * payload.  Unknown future versions are refused rather than silently
 * mis-parsed.
 */
static bool scene_read(JceSnapshotStream *s, uint32_t loaded_version, void *user)
{
    JceScene *scene = (JceScene *)user;
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

bool jce_save_register_scene_provider(JceSnapshotRegistry *reg, JceScene *scene)
{
    if (!reg || !scene) return false;
    jce_snapshot_register(reg, SCENE_PROVIDER_ID, SCENE_PROVIDER_VERSION,
                          scene_write, scene_read, scene);
    return true;
}
