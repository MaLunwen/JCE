/*
 * jce_scene_serial.h  Scene JSON parsing and serialization.
 *
 * Reads/writes JceScene entities and components to/from cJSON trees.
 * Both editor and runtime use this module.
 */

#ifndef JCE_SCENE_SERIAL_FLAT_H
#define JCE_SCENE_SERIAL_FLAT_H


#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_json.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* Public API uses the JceJson facade to avoid leaking <cjson/cJSON.h>
 * from this header.  JceJson is binary-compatible with cJSON, so existing
 * .c TUs that still include cjson directly keep compiling unchanged. */

/* Parse a scene JSON root (flat entity array format) into an existing scene.
 * Entities are appended to the scene; existing entities are not cleared.
 * Handles contract envelope, flat array, and legacy formats.
 * Returns the number of entities loaded, or -1 on error. */
JCE_API int jce_scene_load_json(JceScene *scene, const JceJson *root);

/* Parse the "components" array from a single entity JSON object into
 * the given entity. Accepts both a "components" array with "type" fields
 * and all key aliases / component types the engine recognizes.
 * Editor-specific fields (name, tag, enabled, etc.) on the entity object
 * are applied as EditorMeta if present. */
void jce_scene_parse_entity_json(JceScene *scene, JceEntity e,
                                 const JceJson *entity_obj);

/* Serialize all components of a single entity into a JSON array.
 * Caller owns the returned tree and must release it with jce_json_free(). */
JCE_API JceJson *jce_scene_serialize_entity_components(JceScene *scene, JceEntity e);

/* Serialize all entities in the scene to a JSON root (flat entity array).
 * Caller owns the returned tree and must release it with jce_json_free(). */
JCE_API JceJson *jce_scene_save_json(const JceScene *scene);

/* Serialize a subtree (root + all transitive descendants in the
 * parent/child hierarchy) to a JSON root. Used by the prefab system.
 * Caller owns the returned tree and must release it with jce_json_free(). */
JCE_API JceJson *jce_scene_save_subtree_json(const JceScene *scene, JceEntity root);

/* Set the base directory used to resolve sibling material references
 * during the next jce_scene_load_json() call.
 * Pass NULL to clear. The resource-layer wrapper sets this around its
 * VFS loads; runtime PAK loads typically leave it empty. */
JCE_API void jce_scene_serial_set_base_dir(const char *dir);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_SERIAL_FLAT_H */
