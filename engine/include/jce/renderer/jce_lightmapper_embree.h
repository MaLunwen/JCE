/*
 * jce_lightmapper_embree.h  Optional Embree backend shim.
 *
 * Compile-time gate.  When `JCE_HAS_EMBREE` is defined at build time,
 * the lightmapper bake (`jce_lightmapper_bake.c`) substitutes its
 * built-in median-split BVH for Intel Embree's BVH4 / BVH8 builder
 * + rtcIntersect1.
 *
 * The header is intentionally Embree-free: it only declares the
 * minimal hooks the bake module needs, leaving the actual <embree4>
 * include + RTCDevice handling to a host TU added in a downstream
 * branch (typically `engine/src/renderer/jce_lightmapper_embree.c`
 * gated by the same flag in CMake).
 *
 * Default JCE build does NOT define JCE_HAS_EMBREE → the helpers
 * here resolve to inline no-ops + the bake module falls back to
 * built-in BVH.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_LIGHTMAPPER_EMBREE_H
#define JCE_LIGHTMAPPER_EMBREE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when Embree is compiled in.  Bake module branches on this. */
#ifdef JCE_HAS_EMBREE
#define JCE_LIGHTMAPPER_EMBREE_ENABLED 1
#else
#define JCE_LIGHTMAPPER_EMBREE_ENABLED 0
#endif

/* Opaque handles — real types live behind the impl TU when enabled. */
typedef struct JceEmbreeScene  JceEmbreeScene;
typedef struct JceEmbreeDevice JceEmbreeDevice;

/* Lifecycle.  All return NULL / false when Embree isn't compiled in. */
JceEmbreeDevice *jce_embree_create_device(void);
void             jce_embree_destroy_device(JceEmbreeDevice *d);

JceEmbreeScene  *jce_embree_create_scene(JceEmbreeDevice *d);
void             jce_embree_destroy_scene(JceEmbreeScene  *s);

/* Add a triangle mesh.  Indices interpreted as triangles
 * (triangle_count*3 entries).  Returns the geometry id. */
uint32_t jce_embree_add_triangle_mesh(JceEmbreeScene *s,
                                       const float    *positions,
                                       uint32_t        vertex_count,
                                       const uint32_t *indices,
                                       uint32_t        triangle_count);

void     jce_embree_commit(JceEmbreeScene *s);

/* Single-ray any-hit query.  Returns true if any triangle is hit
 * within `max_t`.  Used by the lightmap AO loop. */
bool     jce_embree_occluded(JceEmbreeScene *s,
                              const float ro[3], const float rd[3],
                              float max_t);

#ifndef JCE_HAS_EMBREE
/* No-op stubs so referring TUs link without Embree at all. */
static inline JceEmbreeDevice *jce_embree_create_device(void)  { return NULL; }
static inline void             jce_embree_destroy_device(JceEmbreeDevice *d) { (void)d; }
static inline JceEmbreeScene  *jce_embree_create_scene(JceEmbreeDevice *d) { (void)d; return NULL; }
static inline void             jce_embree_destroy_scene(JceEmbreeScene *s) { (void)s; }
static inline uint32_t jce_embree_add_triangle_mesh(JceEmbreeScene *s,
                                                     const float *p, uint32_t vc,
                                                     const uint32_t *i, uint32_t tc)
{ (void)s; (void)p; (void)vc; (void)i; (void)tc; return 0xFFFFFFFFu; }
static inline void     jce_embree_commit(JceEmbreeScene *s) { (void)s; }
static inline bool     jce_embree_occluded(JceEmbreeScene *s, const float ro[3],
                                            const float rd[3], float mt)
{ (void)s; (void)ro; (void)rd; (void)mt; return false; }
#endif

#ifdef __cplusplus
}
#endif

#endif /* JCE_LIGHTMAPPER_EMBREE_H */
