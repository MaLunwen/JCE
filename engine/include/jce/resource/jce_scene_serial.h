/*
 * jce_scene_serial.h  Scene serialization (JSON via cJSON).
 *
 * Saves and loads the full ECS scene graph to/from a JSON file.
 * Each entity is stored with its name and all known components
 * (transform, mesh renderer, camera, light, etc.).
 *
 * The format is human-readable and diff-friendly, making it
 * suitable for version control integration.
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
typedef struct JceScene JceScene;

/* ================================================================== */
/* Save                                                                */
/* ================================================================== */

/* Serialize the scene to a JSON string.
   Returns a heap-allocated NUL-terminated string.
   Caller must free the string with jce_scene_serial_free(). */
char *jce_scene_serial_save(const JceScene *scene, size_t *out_len);

/* Write the scene to a file path (UTF-8 JSON). */
bool  jce_scene_serial_save_file(const JceScene *scene, const char *path);

/* ================================================================== */
/* Load                                                                */
/* ================================================================== */

/* Deserialize a scene from a JSON string.
   Existing entities in 'scene' are cleared before loading.
   Returns true on success. */
bool jce_scene_serial_load(JceScene *scene, const char *json, size_t len);

/* Load the scene from a file path. */
bool jce_scene_serial_load_file(JceScene *scene, const char *path);

/* ================================================================== */
/* Memory                                                              */
/* ================================================================== */

/* Free a string returned by jce_scene_serial_save(). */
void jce_scene_serial_free(char *json);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SCENE_SERIAL_H */
