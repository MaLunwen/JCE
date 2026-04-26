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

#ifdef __cplusplus
}
#endif

#endif /* JCE_SHADER_MANAGER_H */
