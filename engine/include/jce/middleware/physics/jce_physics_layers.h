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
 * JSON I/O is the authoring → shipped-game bridge, NOT an unread write
 * path: the editor build exports the project's authored matrix as
 * `<cooked>/physics_layers.json` and the shipped runtime loads it back in
 * app_init — loose cooked tree first, then the embedded PAK via the _mem
 * variant — before any body spawns.  Editor Play needs no file at all: it
 * pushes the same authored matrix straight into this cache.
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

/* ── The 2D matrix ────────────────────────────────────────────────── *
 * SEPARATE MATRIX, SHARED NAMES.  The 32 slots and their names are one
 * vocabulary -- "Player" means the same thing to both worlds -- but which
 * pairs collide is a per-world decision, exactly as Unity splits Physics from
 * Physics2D.  That is not a preference: JceProjectSettings already authors
 * BOTH (physics.layer_collision_matrix and physics2d.layer_collision_matrix)
 * and the physics debugger draws both grids, so one shared matrix would
 * delete a distinction the authoring surface already exposes.
 *
 * UNTIL 2026-09-21 THE 2D MATRIX REACHED NOTHING.  jce_physics2d.c contained
 * zero uses of b2Filter / categoryBits / maskBits, JceBody2DDesc had no layer
 * field, and the editor forwarded only gravity2d out of JceProjectPhysics2D.
 * An author edited a 32x32 grid, it was persisted and shipped, and every 2D
 * body collided with every other one.  The 3D sibling has been wired this
 * whole time, which is what makes it a gap rather than a decision. */
JCE_API void                JCE_CALL jce_physics2d_set_layer_collides(
                                uint32_t layer_a, uint32_t layer_b,
                                bool collides);
JCE_API bool                JCE_CALL jce_physics2d_get_layer_collides(
                                uint32_t layer_a, uint32_t layer_b);

/* Row mask for `layer` in the 2D matrix; bit j set = collides with layer j.
 * Out-of-range returns 0.  This is what jce_physics2d_body_create turns into
 * b2Filter.maskBits -- 2D resolves the filter AT CREATION because Box2D takes
 * it in b2ShapeDef, unlike the 3D path which can apply it to a live body. */
JCE_API JcePhysicsLayerMask JCE_CALL jce_physics2d_get_layer_collision_mask(
                                uint32_t layer);

/* ── JSON I/O ─────────────────────────────────────────────────────── *
 * Schema (jce.physlayers.v1):
 *   { "$schema": "jce.physlayers.v1",
 *     "names":   [ "Default", "Layer 1", ..., "Layer 31" ],
 *     "matrix":  [ 4294967295, 4294967295, ... ]  // 32 row masks
 *   }
 * The file-backed pair uses jce_fs_host_* under the hood (no raw C
 * runtime).  Returns false on I/O or parse error.
 * save_json's output is read back by the loaders below in every shipped
 * build — do not retire it as a write-only path. */
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
