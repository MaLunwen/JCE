/*
 * jce_material_registry.h  Dev-mode hot-reload tracker for .mat.json.
 *
 * A tiny watcher that lives ONLY when the engine is launched with
 * `--dev <assets_dir>` (see jce_args.h).  In shipping builds it
 * silently degrades to a no-op so consumers can call into it
 * unconditionally.
 *
 * Lifecycle (typical):
 *   jce_material_registry_init(dev_dir);          // dev_dir from the app
 *                                                 // layer (jce_args), or NULL
 *   jce_material_registry_set_reload_cb(cb, ud);  // game wires its
 *                                                 // own "patch all
 *                                                 // MeshRenderers
 *                                                 // matching path"
 *                                                 // closure
 *   jce_material_registry_track("Materials/A.mat.json");  // per asset
 *   ...
 *   jce_material_registry_poll(dt);   // every frame, internally throttled
 *   ...
 *   jce_material_registry_shutdown(); // engine teardown
 *
 * Tracking the same path twice is a no-op.  Capacity is fixed at
 * compile time — overflow is logged once and ignored.
 *
 * Thread model: single-threaded.  Call from the main thread only.
 *
 * Layer: L3 renderer (depends on jce_pbr_material + jce_filesystem).
 * The dev-assets dir is passed IN by the caller (app/game layer owns jce_args)
 * so this L3 module never reaches up into the application layer.
 */

#ifndef JCE_MATERIAL_REGISTRY_H
#define JCE_MATERIAL_REGISTRY_H

#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_pbr_material.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Reload callback: invoked when a tracked material's host file mtime
 * advances and the new contents parse successfully.
 *   vfs_path : the path used at register time, e.g. "Materials/A.mat.json"
 *   pbr      : freshly-parsed PBR factor block
 *   tex_paths: 5 texture path slots (albedo, mr, normal, ao, emissive),
 *              already in vfs convention; empty string means "unset"
 *   user     : opaque pointer passed to set_reload_cb()
 * The callback must NOT call back into the registry. */
typedef void (*JceMaterialReloadFn)(const char *vfs_path,
                                    const JcePbrMaterial *pbr,
                                    const char tex_paths[5][256],
                                    void *user);

/* Initialise with the dev-assets directory (typically
 * jce_args_get_dev_assets() from the app/game layer).  Pass NULL or "" to run
 * as a no-op tracker (shipping builds).  Returns true even in no-op mode. */
JCE_API bool JCE_CALL jce_material_registry_init(const char *dev_assets_dir);

/* Release internal state. */
JCE_API void JCE_CALL jce_material_registry_shutdown(void);

/* Register a hot-reload callback (single slot, last call wins).
 * Pass NULL to clear. */
JCE_API void JCE_CALL jce_material_registry_set_reload_cb(
    JceMaterialReloadFn cb, void *user);

/* Start tracking a material file.  No-op in non-dev mode, or if the
 * path is already tracked, or if the host file does not exist. */
JCE_API void JCE_CALL jce_material_registry_track(const char *vfs_path);

/* Forget every tracked material — typically called on scene unload so
 * callbacks don't fire against torn-down entities. */
JCE_API void JCE_CALL jce_material_registry_clear(void);

/* Frame tick.  Internally throttled to ~2 Hz; cheap to call every
 * frame.  Pass elapsed seconds since the previous call. */
JCE_API void JCE_CALL jce_material_registry_poll(double dt_sec);

JCE_EXTERN_C_END

#endif /* JCE_MATERIAL_REGISTRY_H */
