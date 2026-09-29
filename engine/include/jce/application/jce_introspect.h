/*
 * jce_introspect.h -- machine-readable descriptions of what the engine
 * accepts and what it is currently doing.
 *
 * WHY THIS EXISTS.  A program driving JCE -- an external agent, an IDE, a
 * script -- has to be able to ASK what components exist, what fields they
 * have, and what is in the scene, instead of being told by a document that
 * will be wrong within a month.  Everything here answers in UTF-8 JSON,
 * because the caller is on the other side of a process boundary.
 *
 * THE COMPONENT SCHEMA IS NOT A TABLE.  It is produced by round-tripping the
 * engine's OWN serialiser: for each registered component type, an empty JSON
 * object is handed to that type's parse function -- which fills in the
 * engine's defaults, since every field read is `j_num(c, "fov", 60.0)` and
 * friends -- and the result is then handed straight back to that type's
 * serialise function.  The keys that come out ARE the fields, and the values
 * that come out ARE the defaults, both from the same code the editor's
 * Inspector and every .scene.json on disk already go through.
 *
 * That is the whole point.  A second table describing the components would
 * drift from the serialiser, and the drift would be invisible: both halves
 * keep working, and only a scene authored against the stale half is wrong --
 * silently, because the loader DISCARDS an unknown field without a word.
 * ADR-08 and REQ-INT-02 require single sourcing; deriving it costs about
 * sixty lines and makes the requirement structural rather than remembered.
 *
 * SIZING FOLLOWS snprintf.  Every function returns the number of bytes the
 * answer needs, EXCLUDING the terminating NUL, whether or not it fit.  Call
 * with cap == 0 (buf may be NULL) to size the buffer, allocate, call again.
 * A truncated answer is still NUL-terminated when cap > 0, and a caller that
 * ignores the return value gets valid-but-short text rather than a buffer
 * overrun -- but it will not be parseable JSON, which is the loud failure
 * this shape is chosen for.
 *
 * NO cJSON IN THIS HEADER.  Public headers carry no third-party type
 * (check_public_api_purity.py), so the interface is char buffers even though
 * the implementation builds a cJSON tree.
 */

#ifndef JCE_INTROSPECT_H
#define JCE_INTROSPECT_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_defs.h>

#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Every registered component type: canonical JSON name, accepted aliases,
 * whether it can be added and serialised through the generic path, and --
 * for those that can -- every field with the engine's own default value.
 *
 * Shape:
 *   {"engine_version": "...", "component_count": N,
 *    "components": [
 *      {"name": "Rigidbody", "aliases": ["rigidbody"], "id": 12,
 *       "legacy_flag": "0x...", "struct_size": 96,
 *       "authorable": true, "fields": {"mass": 1.0, ...}},
 *      {"name": "Light", "authorable": false,
 *       "not_authorable_because": "..."} ]}
 *
 * A row with "authorable": false is reported rather than omitted: the caller
 * needs to tell "this component does not exist" from "this component exists
 * and cannot be written through this path".  Those are different answers and
 * an omission collapses them into one. */
JCE_API size_t JCE_CALL jce_introspect_components_json(char *buf, size_t cap);

/* The entity tree of a loaded scene: id, name, parent and the component type
 * names each entity carries.  depth < 0 means the whole tree.
 *
 * Values are deliberately absent -- a scene of twenty thousand entities with
 * every field of every component is not an overview.  Use
 * jce_introspect_entity_json for one entity's values. */
JCE_API size_t JCE_CALL jce_introspect_scene_json(const JceScene *scene,
                                                  int depth,
                                                  char *buf, size_t cap);

/* One entity: its name, parent, and every component with every field value,
 * emitted by the same serialiser that writes .scene.json. */
JCE_API size_t JCE_CALL jce_introspect_entity_json(const JceScene *scene,
                                                   JceEntity entity,
                                                   char *buf, size_t cap);

/* A snapshot of what this process is doing: engine version, entity count,
 * memory, and per-phase frame timings when the profiler is on.
 *
 * A MEASUREMENT THE ENGINE DOES NOT HAVE IS FLAGGED, NEVER ZEROED.  Each one
 * carries a `<name>_available` boolean, and when that is false the value key
 * is ABSENT and `<name>_absent_because` says why:
 *
 *   {"frame_phases_available": false,
 *    "frame_phases_absent_because": "the profiler is off (...)"}
 *
 * Zero is a number a caller compares against a budget and concludes everything
 * is fast.  "There was nothing to measure" is a different fact and has to look
 * different. */
JCE_API size_t JCE_CALL jce_introspect_stats_json(const JceScene *scene,
                                                  char *buf, size_t cap);

JCE_EXTERN_C_END

#endif /* JCE_INTROSPECT_H */
