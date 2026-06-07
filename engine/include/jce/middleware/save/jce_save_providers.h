/*
 * jce_save_providers.h  Built-in snapshot providers.
 *
 * Glue between the generic jce_snapshot registry and the concrete engine
 * subsystems a save file needs to capture.  The snapshot framework itself
 * (jce_snapshot.h) is deliberately generic — it knows nothing about ECS,
 * scenes, or gameplay.  This header registers the standard providers so a
 * game runtime can persist + restore a play session with one call each.
 *
 * Provider section IDs (stable — do not rename across versions):
 *   "scene_ecs"  v1   full scene/ECS state (every authored entity +
 *                     component), serialized through the shared scene JSON
 *                     schema (scene/jce_scene_components_json.c).  On load
 *                     the scene is cleared and rehydrated from the section.
 *
 * Typical use (game runtime startup):
 *   JceSnapshotRegistry *reg = jce_snapshot_registry_create();
 *   jce_save_register_scene_provider(reg, scene);
 *   ...
 *   jce_snapshot_save_to_file(reg, "saves/slot0.jsnp");   // checkpoint
 *   jce_snapshot_load_from_file(reg, "saves/slot0.jsnp");  // restore
 *
 * Layer: middleware/save (L4).
 */
#ifndef JCE_SAVE_PROVIDERS_H
#define JCE_SAVE_PROVIDERS_H

#include <jce/middleware/save/jce_snapshot.h>
#include <jce/os/core/jce_defs.h>
#include <stdbool.h>

JCE_EXTERN_C_BEGIN

typedef struct JceScene JceScene;

/*
 * Register the scene/ECS snapshot provider on `reg` under section id
 * "scene_ecs".  The write callback serializes the whole scene through the
 * shared component-JSON schema; the read callback clears the scene and
 * reloads every entity from the section payload.  `scene` must outlive any
 * save/load call routed through `reg`.  No-op when either argument is NULL.
 *
 * Returns true when the provider was registered.
 */
JCE_API bool jce_save_register_scene_provider(JceSnapshotRegistry *reg,
                                              JceScene            *scene);

JCE_EXTERN_C_END
#endif /* JCE_SAVE_PROVIDERS_H */
