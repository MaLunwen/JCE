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

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations. */
typedef struct JceScene      JceScene;
typedef struct JceFileSystem JceFileSystem;

/* ================================================================== */
/* Save                                                                */
/* ================================================================== */

/* Serialize the scene to a JSON string.
   Returns a heap-allocated NUL-terminated string.
   Caller must free the string with jce_scene_serial_free(). */
char *jce_scene_serial_save(const JceScene *scene, size_t *out_len);

/* Write the scene to a file path (UTF-8 JSON).
   Uses the engine VFS for cross-platform I/O.
   path is relative to the write directory set on fs. */
bool  jce_scene_serial_save_file(const JceScene *scene, const char *path);

/* ================================================================== */
/* Load                                                                */
/* ================================================================== */

/* Deserialize a scene from a JSON string.
   Existing entities in 'scene' are cleared before loading.
   Returns true on success. */
bool jce_scene_serial_load(JceScene *scene, const char *json, size_t len);

/* Load the scene from a file path.
   Uses the engine VFS for cross-platform I/O.
   Falls back to the native filesystem if fs is NULL. */
bool jce_scene_serial_load_file(JceScene *scene, const char *path);

/* Load the scene from a virtual path through the VFS.
   Supports reading from PAK archives and mounted directories. */
bool jce_scene_serial_load_vfs(JceScene *scene,
                               const JceFileSystem *fs,
                               const char *virtual_path);

/* ================================================================== */
/* Memory                                                              */
/* ================================================================== */

/* Free a string returned by jce_scene_serial_save(). */
void jce_scene_serial_free(char *json);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SCENE_SERIAL_H */
