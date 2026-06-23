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

/* ── Incremental (frame-sliced) scene load ───────────────────────────
 *
 * jce_scene_load_json() does all its work in one call.  For very large
 * scenes (tens of thousands of entities) the entity-create + component-
 * parse first pass dominates and can block the caller for seconds.  The
 * streaming API below splits that exact same work across multiple calls
 * so an interactive host can keep its frame loop alive and show progress.
 *
 * Contract: identical end-state to jce_scene_load_json() — the same
 * entities, components, parent links, and reference fixups.  In fact
 * jce_scene_load_json() is implemented as begin + step(all) + finalize.
 *
 * Usage:
 *     JceSceneLoadStream *st = jce_scene_load_stream_begin(scene, root);
 *     if (!st) { handle error / empty scene; }
 *     while (!jce_scene_load_stream_done(st))
 *         jce_scene_load_stream_step(st, 1500);   // N entities/frame
 *     int n = jce_scene_load_stream_finalize(st); // runs ref fixups, frees
 *
 * The stream does NOT own `scene` or `root`; the caller must keep both
 * alive until finalize returns.  Entities are appended (additive), same
 * as jce_scene_load_json().  Aborting mid-stream: call finalize() at any
 * time to run the fixups over whatever was created so far and release the
 * stream (entities created so far remain in the scene). */
typedef struct JceSceneLoadStream JceSceneLoadStream;

/* Begin an incremental load.  Parses the scene-level rendering/streaming
 * settings immediately (cheap) and prepares the entity cursor + remap
 * table.  Returns NULL on error (bad args, no/empty entity array, OOM) —
 * in that case there is nothing to step or finalize.  out_total, if
 * non-NULL, receives the total entity count for progress display. */
JCE_API JceSceneLoadStream *jce_scene_load_stream_begin(JceScene *scene,
                                                        const JceJson *root,
                                                        int *out_total);

/* Create + parse up to max_entities more entities (the first pass).
 * max_entities <= 0 means "all remaining".  Returns the number of
 * entities processed so far (cumulative). */
JCE_API int jce_scene_load_stream_step(JceSceneLoadStream *st, int max_entities);

/* True once every entity has been created (first pass complete). */
JCE_API bool jce_scene_load_stream_done(const JceSceneLoadStream *st);

/* Cumulative entities processed so far, and the total. */
JCE_API int jce_scene_load_stream_processed(const JceSceneLoadStream *st);
JCE_API int jce_scene_load_stream_total(const JceSceneLoadStream *st);

/* Copy the ids of all entities created so far (this stream's roster, in
 * creation order) into out_ids, up to cap entries, and return the total
 * number created.  Pass out_ids = NULL to query the count without copying;
 * then call again with a buffer >= the returned size.  This lets a streaming
 * host build its per-chunk entity roster in O(new) rather than diffing the
 * whole scene.  Valid until jce_scene_load_stream_finalize() frees the
 * stream. */
JCE_API uint32_t jce_scene_load_stream_new_entities(
    const JceSceneLoadStream *st, JceEntity *out_ids, uint32_t cap);

/* Run the second-pass parent/reference fixups over all created entities,
 * end the material cache, free the stream.  Returns the number of
 * entities loaded (>= 0).  The stream pointer is invalid afterwards. */
JCE_API int jce_scene_load_stream_finalize(JceSceneLoadStream *st);

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
