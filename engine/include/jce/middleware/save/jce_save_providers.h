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
#include <jce/middleware/save/jce_save_migration.h>
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

/*
 * Same as above but wires a migration registry the scene read path consults
 * to upgrade older saves to the current "scene_ecs" schema before loading
 * them.  Pass the registry you populated with jce_save_migration_register()
 * for section id "scene_ecs".  `migrations` may be NULL (then an older save
 * with no built-in path is refused, as before).  Re-registering (e.g. on a
 * scene reload) re-points the existing provider context in place without
 * leaking.  `scene` and `migrations` must outlive any save/load on `reg`.
 */
JCE_API bool jce_save_register_scene_provider_ex(JceSnapshotRegistry      *reg,
                                                 JceScene                 *scene,
                                                 JceSaveMigrationRegistry *migrations);

/*
 * Remove the "scene_ecs" provider from `reg` and free the heap context the
 * register helpers allocated for it.  Call before destroying `reg` when you
 * used the register helpers (jce_snapshot_registry_destroy alone does not
 * know about the provider context).  No-op on NULL.
 */
JCE_API void jce_save_unregister_scene_provider(JceSnapshotRegistry *reg);

/*
 * Register the scene ENVIRONMENT snapshot provider on `reg` under section id
 * "scene_env".
 *
 * The scene_ecs section above captures authored data through the scene JSON
 * schema, and the live environment is deliberately not in that schema: writing
 * it there would put a running clock into the authored scene file, so saving a
 * level from the editor would bake whatever hour the preview had reached.  This
 * is the other half -- four ACCUMULATED values that a session earns and cannot
 * re-derive:
 *
 *   the hour of day, the monotonic world_time_seconds behind it, and the
 *   global_wetness / snow_amount integrators
 *
 * Everything else in JceEnvironmentState is recomputed from the authored
 * settings on the next advance (weather, wind, humidity, temperature, and the
 * sun placed from the hour), so persisting it would be persisting a cache.
 *
 * A save written before this section existed simply lacks it, and the snapshot
 * loader skips unknown sections -- so an old save loads and the hour falls back
 * to the scene's authored tod_hour.  No version bump, no migration entry.
 *
 * `scene` must outlive any save/load call routed through `reg`.  The scene
 * pointer IS the user data, so there is no context to free and no unregister
 * counterpart: jce_snapshot_unregister(reg, "scene_env") is enough.
 *
 * Returns true when the provider was registered.
 */
JCE_API bool jce_save_register_env_provider(JceSnapshotRegistry *reg,
                                            JceScene            *scene);

JCE_EXTERN_C_END
#endif /* JCE_SAVE_PROVIDERS_H */
