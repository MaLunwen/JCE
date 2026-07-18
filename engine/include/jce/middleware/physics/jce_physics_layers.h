/*
 * jce_physics_layers.h  32-slot Physics Layer Collision Matrix
 *                       (Unity-parity, P3-C.2).
 *
 * A process-wide cache of 32 layer names + a symmetric 32x32 boolean
 * collision matrix that decides which physics layers may collide with
 * which.  Mirrors Unity's `Edit > Project Settings > Physics > Layer
 * Collision Matrix`.
 *
 * The matrix is stored as 32 row bitmasks (uint32_t); bit `j` of row
 * `i` is set when layer `i` collides with layer `j`.  Set operations
 * are always symmetric: `set(a, b, x)` updates both `(a,b)` and
 * `(b,a)`.  Defaults: every pair collides (rows = 0xFFFFFFFF), layer
 * 0 is named "Default" and the remaining slots hold "Layer N".
 *
 * JSON I/O writes the canonical `.jce/physics_layers.json` document
 * consumed by both the editor panel and runtime body filter helpers.
 *
 * Layer: middleware/physics (L4).  Pure C99.
 */

#ifndef JCE_PHYSICS_LAYERS_H
#define JCE_PHYSICS_LAYERS_H

#include <jce/middleware/physics/jce_physics.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_PHYSICS_LAYER_COUNT 32

/* Bitmask over the 32 physics layers (bit i = layer i). */
typedef uint32_t JcePhysicsLayerMask;

/* ── Layer naming ─────────────────────────────────────────────────── */

/* Up to 63 ASCII characters; trailing characters are truncated.
 * Passing NULL clears the slot (treated as anonymous). */
JCE_API void        JCE_CALL jce_physics_layer_set_name(uint32_t layer_index,
                                                        const char *name);
/* Never returns NULL.  Returns "" if the slot is empty. */
JCE_API const char *JCE_CALL jce_physics_layer_get_name(uint32_t layer_index);

/* ── 32x32 symmetric matrix ───────────────────────────────────────── */

JCE_API void                JCE_CALL jce_physics_set_layer_collides(
                                uint32_t layer_a, uint32_t layer_b,
                                bool collides);
JCE_API bool                JCE_CALL jce_physics_get_layer_collides(
                                uint32_t layer_a, uint32_t layer_b);

/* Returns the row mask for `layer`: bit j is set when layer collides
 * with layer j.  Out-of-range index returns 0. */
JCE_API JcePhysicsLayerMask JCE_CALL jce_physics_get_layer_collision_mask(
                                uint32_t layer);

/* All pairs collide, names reset to "Default" / "Layer N". */
JCE_API void JCE_CALL jce_physics_layer_matrix_reset_default(void);

/* ── JSON I/O ─────────────────────────────────────────────────────── *
 * Schema (jce.physlayers.v1):
 *   { "$schema": "jce.physlayers.v1",
 *     "names":   [ "Default", "Layer 1", ..., "Layer 31" ],
 *     "matrix":  [ 4294967295, 4294967295, ... ]  // 32 row masks
 *   }
 * Both functions use jce_fs_host_* under the hood (no raw C runtime).
 * Returns false on I/O or parse error. */
JCE_API bool JCE_CALL jce_physics_layer_matrix_save_json(const char *vfs_path);
JCE_API bool JCE_CALL jce_physics_layer_matrix_load_json(const char *vfs_path);
/* Parse from an in-memory JSON buffer (single-exe: bytes decompressed from the
 * embedded PAK).  `len` may be 0 to strlen(json). */
JCE_API bool JCE_CALL jce_physics_layer_matrix_load_json_mem(const char *json,
                                                             size_t len);

/* ── Body integration helper ──────────────────────────────────────── *
 * Apply the current matrix row for `layer_index` to an existing body
 * via the collision-filter path (group = 1<<layer_index, mask = full
 * 32-bit matrix row).  All 32 layers are addressable.  No-op when
 * world/body is NULL or layer_index >= JCE_PHYSICS_LAYER_COUNT. */
JCE_API void JCE_CALL jce_physics_body_set_layer(JcePhysicsWorld *world,
                                                 JceBodyHandle body,
                                                 uint32_t layer_index);

JCE_EXTERN_C_END

#endif /* JCE_PHYSICS_LAYERS_H */
