/*
 * jce_decals.h -- runtime surface-decal pool.
 *
 * Decals are textured oriented quads that "stamp" onto world surfaces:
 * bullet holes, blood splatters, scorch marks, footprints, graffiti.
 *
 * This implementation places each decal as a single thin quad oriented
 * by the surface normal at hit point.  No depth-buffer reconstruction
 * required — works in the existing forward pipeline and survives MSAA
 * cleanly.  For perfect concavity hugging (e.g. wrapping around corners)
 * upgrade later to deferred box-projection decals.
 *
 * Lifetime model: each decal carries a TTL.  When TTL expires, the slot
 * is reclaimed.  Spawning past capacity overwrites the oldest live decal.
 *
 * Layer: Graphics (Layer 3) — public.
 */

#ifndef JCE_DECALS_H
#define JCE_DECALS_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_texture_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceDecalPool  JceDecalPool;
typedef struct JcePakArchive JcePakArchive;

/* Per-decal spawn parameters. */
typedef struct {
    jce_vec3   position;       /* world hit point */
    jce_vec3   normal;          /* unit world surface normal */
    jce_vec3   tangent_hint;    /* optional roll axis (zero = auto) */
    float      size;            /* world-space side length (square) */
    float      thickness;       /* small offset along normal to avoid z-fight */

    JceTexture texture;         /* RGBA decal texture */
    jce_vec4   tint;            /* multiplied with texture (1,1,1,1 = pass-through) */

    float      lifetime_seconds;/* 0 = persistent until evicted */
} JceDecalSpawn;

/* System-wide configuration. */
typedef struct {
    uint32_t              max_decals;   /* pool capacity (rounded to >=16) */
    const JcePakArchive  *pak;          /* loads vs_decal / fs_decal shaders */
} JceDecalPoolDesc;

JCE_API JceDecalPool *jce_decals_create(const JceDecalPoolDesc *desc);
JCE_API void           jce_decals_destroy(JceDecalPool *pool);

/* Spawn a decal.  Returns true on success.  Idempotent NULL/zero-size guard. */
JCE_API bool jce_decals_spawn(JceDecalPool *pool, const JceDecalSpawn *spawn);

/* Advance lifetimes by `dt` seconds and reclaim expired slots. */
JCE_API void jce_decals_update(JceDecalPool *pool, float dt);

/* Submit all live decals to the given bgfx view.  Caller is responsible
 * for view setup (camera matrices, viewport).  Decals draw with alpha
 * blending and depth read enabled, depth write disabled. */
JCE_API void jce_decals_render(JceDecalPool *pool, uint16_t view_id);

/* Drop all decals (e.g. on level reload). */
JCE_API void jce_decals_clear(JceDecalPool *pool);

JCE_API uint32_t jce_decals_live_count(const JceDecalPool *pool);
JCE_API uint32_t jce_decals_capacity(const JceDecalPool *pool);

JCE_EXTERN_C_END

#endif /* JCE_DECALS_H */
