/*
 * jce_tilemap_collider_2d.h  Generate Box2D shapes from a Tilemap.
 *
 * Unity TilemapCollider2D analog.  Iterates each non-empty cell in a
 * `JceTilemap`, runs a coalescing pass to merge horizontally
 * adjacent cells into a single wide box (reduces shape count), and
 * emits an array of `JceTilemapColliderShape` rectangles in world
 * coordinates.
 *
 * No Box2D / Bullet linkage here — output is a plain shape list the
 * caller hands to the physics middleware.
 *
 * Layer: physics (Layer 4) — public.
 */

#ifndef JCE_TILEMAP_COLLIDER_2D_H
#define JCE_TILEMAP_COLLIDER_2D_H

#include <jce/renderer/jce_tilemap.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_TILEMAP_COLLIDER_SHAPES_MAX 4096

typedef struct {
    /* World-space AABB. */
    float x, y, w, h;
    /* Optional friction / restitution overrides; 0 = inherit map. */
    float friction;
    float restitution;
    bool  is_trigger;
} JceTilemapColliderShape;

typedef struct {
    /* If non-zero, only cells with sprite_index matching this filter
     * mask contribute (bit per low-byte of sprite_index). */
    uint32_t sprite_mask;
    /* Friction / restitution applied to every emitted shape unless
     * overridden via per-shape post-edit. */
    float    default_friction;
    float    default_restitution;
    /* When true, run row-coalescing to merge runs of cells into a
     * single wider box. */
    bool     coalesce_rows;
} JceTilemapColliderOptions;

/* Generate shapes from `map` and write up to `cap` into `out`.
 * Returns number actually written.  Designed to be called on tilemap
 * load + on cell edits; small enough for editor-time rebuild. */
JCE_API uint32_t jce_tilemap_collider_2d_rebuild(
    const JceTilemap                *map,
    const JceTilemapColliderOptions *opts,
    JceTilemapColliderShape         *out,
    uint32_t                          cap);

JCE_EXTERN_C_END

#endif /* JCE_TILEMAP_COLLIDER_2D_H */
