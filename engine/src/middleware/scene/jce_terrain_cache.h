/*
 * jce_terrain_cache.h -- one loaded terrain per path, shared by every consumer.
 *
 * WHY THIS EXISTS
 * ---------------
 * The same terrain asset was being loaded from disk FIVE separate times, each
 * into a private JceTerrain the loader then owned:
 *
 *   1. the editor's terrain panel        (the authoring copy)
 *   2. the scene renderer's 16-slot cache
 *   3. the runtime, at physics spawn
 *   4. the pick pass
 *   5. foliage/grass (borrowing the renderer's)
 *
 * Five copies of a 1025x1025 height grid is 20 MB of pure duplication, but the
 * memory is the least of it: they are five copies that can DISAGREE.  Sculpt a
 * valley in the editor and the renderer shows it, while the collider the player
 * walks on and the mesh the mouse picks against are still the version that was
 * on disk when the level loaded.  There is no event that fixes this, because
 * each owner loaded independently and none of them knows the others exist.
 *
 * The cache makes "the terrain at this path" a single object with a REVISION.
 * Consumers borrow it and rebuild their derived data -- chunk meshes, splat
 * textures, collision shapes -- when the revision moves.  Derived GPU state
 * stays with each consumer, because that genuinely is theirs; the height grid
 * does not.
 *
 * LOADING is a callback rather than a direct filesystem call.  The renderer
 * resolves paths through its own callbacks, the pick pass through a different
 * struct, and the runtime through a host-path helper -- so the cache stays out
 * of that argument entirely and remains testable with no I/O at all.
 *
 * Layer: Scene (L4) -- PRIVATE.
 */

#ifndef JCE_TERRAIN_CACHE_H
#define JCE_TERRAIN_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceTerrain      JceTerrain;
typedef struct JceTerrainCache JceTerrainCache;

/* Load the terrain at `path`, or return NULL.  The cache takes ownership of a
 * successful result and frees it with jce_terrain_free. */
typedef JceTerrain *(*JceTerrainCacheLoadFn)(void *ud, const char *path);

JceTerrainCache *jce_terrain_cache_create(void);
void jce_terrain_cache_destroy(JceTerrainCache *cache);

/* Get-or-load the terrain at `path`.
 *
 * Returns a BORROWED pointer -- the cache owns it, and a caller that frees it
 * corrupts every other consumer.  Valid until the next invalidate of that path
 * or until the cache is destroyed.
 *
 * A load that FAILS is remembered as a failure and not retried on every frame:
 * a missing terrain would otherwise cost a filesystem miss per consumer per
 * frame forever.  jce_terrain_cache_invalidate clears that memory, so a file
 * that appears later is picked up. */
JceTerrain *jce_terrain_cache_acquire(JceTerrainCache *cache, const char *path,
                                      JceTerrainCacheLoadFn load, void *ud);

/* Already-resident lookup; never loads.  NULL if absent or known-failed. */
JceTerrain *jce_terrain_cache_peek(const JceTerrainCache *cache,
                                   const char *path);

/* Monotonic per-path revision, starting at 1 for a resident terrain and 0 for
 * one that is absent or failed.  Consumers cache derived data against it.
 * Because 0 never denotes a live terrain, "I have not seen this yet" and "this
 * is not loaded" are the same value, which is the answer that makes a consumer
 * rebuild rather than draw something stale. */
uint64_t jce_terrain_cache_revision(const JceTerrainCache *cache,
                                    const char *path);

/* Drop the cached terrain (and any remembered failure) for `path`, so the next
 * acquire reloads and every consumer's revision check fires.  This is what the
 * editor calls after a sculpt/save, and it is the mechanism that did not exist
 * before -- which is why edits never reached the collider or the pick mesh.
 *
 * Passing NULL invalidates EVERY entry. */
void jce_terrain_cache_invalidate(JceTerrainCache *cache, const char *path);

/* Diagnostics: how many terrains are resident right now. */
uint32_t jce_terrain_cache_resident(const JceTerrainCache *cache);

/* The scene owns one; every consumer reaches the terrain through it.  Declared
 * here rather than in the public scene header because the cache is private to
 * the scene layer -- the editor invalidates through a public wrapper. */
struct JceScene;
JceTerrainCache *jce_scene_terrain_cache(struct JceScene *scene);

#ifdef __cplusplus
}
#endif

#endif /* JCE_TERRAIN_CACHE_H */
