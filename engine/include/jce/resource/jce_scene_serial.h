/*
 * jce_scene_serial.h  Scene serialization (JSON format).
 *
 * Saves and loads the full ECS scene graph to/from a JSON file.
 * Each entity is stored with its name and all known components
 * (transform, mesh renderer, camera, light, etc.).
 *
 * The format uses a contract envelope (`contract` + `scene`) and remains
 * human-readable/diff-friendly for version control integration.
 * Legacy root-level scene payloads are accepted on load for compatibility.
 *
 * Layer: Resource (Layer 3).
 */

#ifndef JCE_SCENE_SERIAL_H
#define JCE_SCENE_SERIAL_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_json.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Forward declarations. */
typedef struct JceScene      JceScene;
typedef struct JceFileSystem JceFileSystem;
typedef uint64_t             JceEntity;

/* ================================================================== */
/* Save                                                                */
/* ================================================================== */

/* Serialize the scene to a JSON string.
   Returns a heap-allocated NUL-terminated string.
   Caller must free the string with jce_scene_serial_free(). */
JCE_API char *jce_scene_serial_save(const JceScene *scene, size_t *out_len);

/* Write the scene to a file path (UTF-8 JSON).
   Uses the engine VFS for cross-platform I/O.
   path is relative to the write directory set on fs. */
JCE_API bool  jce_scene_serial_save_file(const JceScene *scene, const char *path);

/* ================================================================== */
/* Load                                                                */
/* ================================================================== */

/* Deserialize a scene from a JSON string.
   Existing entities in 'scene' are cleared before loading.
   Returns true on success. */
JCE_API bool jce_scene_serial_load(JceScene *scene, const char *json, size_t len);

/* Load the scene from a file path.
   Uses the engine VFS for cross-platform I/O.
   Falls back to the native filesystem if fs is NULL. */
JCE_API bool jce_scene_serial_load_file(JceScene *scene, const char *path);

/* Load the scene from a virtual path through the VFS.
   Supports reading from PAK archives and mounted directories. */
JCE_API bool jce_scene_serial_load_vfs(JceScene *scene,
                                       const JceFileSystem *fs,
                                       const char *virtual_path);

/* Apply an already-parsed scene document from a virtual path. This is for
 * asynchronous loaders that parse off-thread and must retain VFS-relative
 * material and texture resolution when they commit on the main thread. */
JCE_API int jce_scene_serial_apply_json_vfs(JceScene *scene,
                                             const JceFileSystem *fs,
                                             const char *virtual_path,
                                             const JceJson *root);

/* ================================================================== */
/* Additive (streaming) load                                           */
/* ================================================================== */

/* Deserialize a scene chunk from a JSON string, appending entities to an
   existing live scene WITHOUT clearing it first.  The caller receives
   ownership of an array of JceEntity handles for all newly created
   entities, which can be used to remove the chunk later.
   Free the array with jce_scene_serial_free_entities().
   Returns true on success. */
JCE_API bool jce_scene_serial_load_additive(JceScene *scene,
                                             const char *json, size_t len,
                                             JceEntity **out_entities,
                                             uint32_t   *out_count);

/* VFS counterpart to jce_scene_serial_load_additive(). The prefab/scene
 * document and all relative material assets resolve through the same mounted
 * filesystem for the duration of the append. */
JCE_API bool jce_scene_serial_load_additive_vfs(JceScene *scene,
                                                 const JceFileSystem *fs,
                                                 const char *virtual_path,
                                                 JceEntity **out_entities,
                                                 uint32_t   *out_count);

/* Free an entity array returned by jce_scene_serial_load_additive(). */
JCE_API void jce_scene_serial_free_entities(JceEntity *entities);

/* ================================================================== */
/* Memory                                                              */
/* ================================================================== */

/* Free a string returned by jce_scene_serial_save(). */
JCE_API void jce_scene_serial_free(char *json);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_SERIAL_H */
