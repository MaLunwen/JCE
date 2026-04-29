/*
 * jce_shader_manager.h  Shader resource cache.
 *
 * Wraps shader_load_program() with a deduplication cache so the
 * same program is never loaded twice.  Pairs with AssetManager
 * for unified resource lifecycle.
 */

#ifndef JCE_SHADER_MANAGER_H
#define JCE_SHADER_MANAGER_H

#include <jce/renderer/jce_gfx_types.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JcePakArchive       JcePakArchive;
typedef struct JceShaderManager JceShaderManager;

/* Create / destroy. */
JceShaderManager *jce_shader_manager_create(const JcePakArchive *pak);
void              jce_shader_manager_destroy(JceShaderManager *mgr);

/* Acquire a shader program by base name (e.g. "pbr").
   Returns cached handle if already loaded. */
JceShaderHandle jce_shader_manager_acquire(JceShaderManager *mgr,
                                            const char *name);

/* Release a shader (ref-count decrement; destroyed at zero). */
void jce_shader_manager_release(JceShaderManager *mgr,
                                 const char *name);

/* Number of loaded programs. */
int jce_shader_manager_count(const JceShaderManager *mgr);

/* ============================================================ */
/* Hot-reload (dev workflow)                                     */
/* ============================================================ */

typedef struct JceFileWatcher JceFileWatcher;

/* Set a filesystem dev directory.  When set, future acquires and
   reload calls will read .bin files from <dev_dir>/shaders/...
   instead of the PAK.  Pass NULL to clear and revert to PAK. */
void jce_shader_manager_set_dev_dir(JceShaderManager *mgr, const char *dev_dir);

/* Re-load a shader by name.  Destroys the old bgfx program and
   creates a new one in its place.  Returns true on success.

   Caveat: bgfx programs cannot be swapped in place — callers that
   cached the OLD JceShaderHandle (e.g., a JceShaderSet snapshot,
   a per-mesh material) will continue to point at the destroyed
   program.  After a successful reload, those callers must call
   jce_shader_manager_acquire(name) again to pick up the new handle.

   The shader manager increments an internal generation counter on
   each successful reload; clients that want to react to reloads
   can poll jce_shader_manager_generation(mgr).
*/
bool     jce_shader_manager_reload(JceShaderManager *mgr, const char *name);
uint32_t jce_shader_manager_generation(const JceShaderManager *mgr);

/* Register every currently-loaded shader (vs_*.bin + fs_*.bin) with
   the watcher.  When the watcher reports a change, the matching
   shader is reloaded automatically.  No-op if dev_dir is NULL.
   Returns the number of files registered. */
int jce_shader_manager_attach_watcher(JceShaderManager *mgr,
                                       JceFileWatcher *watcher);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SHADER_MANAGER_H */
