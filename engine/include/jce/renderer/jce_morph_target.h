/*
 * jce_morph_target.h  Morph-target / BlendShape per-vertex delta storage.
 *
 * A morph target is a per-vertex offset (and optional normal offset)
 * that, when blended into a base mesh by weight w, produces a deformed
 * pose: position[i] = base[i] + Σ(w_k × delta_k[i]).
 *
 * This module owns the data layer:
 *   - JceMorphTarget = (name, vertex_deltas[], normal_deltas[])
 *   - JceMorphSet    = collection of targets attached to a mesh
 *
 * Authoring code (glTF / FBX importer) constructs JceMorphTargets and
 * registers a JceMorphSet against a mesh handle.  Runtime fetches the
 * set via mesh handle and feeds weights to the (future) GPU upload
 * path.
 *
 * Layer: renderer (Layer 5) — public.
 */

#ifndef JCE_MORPH_TARGET_H
#define JCE_MORPH_TARGET_H

#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_gfx_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_MORPH_NAME_LEN 48

typedef struct {
    char     name[JCE_MORPH_NAME_LEN];
    uint32_t vertex_count;
    /* Tightly-packed xyz floats; both arrays are vertex_count*3 long.
     * normal_deltas may be NULL when only position deltas are
     * authored (the importer can synthesise normals at runtime). */
    float   *position_deltas;   /* owned by set */
    float   *normal_deltas;     /* may be NULL */
} JceMorphTarget;

typedef struct JceMorphSet JceMorphSet;

/* Create an empty set sized for `vertex_count` base-mesh vertices.
 * All targets attached must match this vertex count.  Returns NULL on
 * OOM. */
JCE_API JceMorphSet *jce_morph_set_create(uint32_t vertex_count);
JCE_API void         jce_morph_set_destroy(JceMorphSet *set);

JCE_API uint32_t  jce_morph_set_vertex_count(const JceMorphSet *set);
JCE_API uint32_t  jce_morph_set_target_count(const JceMorphSet *set);

/* Attach a target.  Vertex/normal-delta arrays are copied internally
 * (caller can free).  Pass NULL `normal_deltas` if only positions are
 * authored.  Returns the target index, or UINT32_MAX on failure. */
JCE_API uint32_t jce_morph_set_add(JceMorphSet  *set,
                                   const char   *name,
                                   const float  *position_deltas,
                                   const float  *normal_deltas);

/* Look up a target by name (case-sensitive).  Returns the index or
 * UINT32_MAX if not found. */
JCE_API uint32_t jce_morph_set_find(const JceMorphSet *set, const char *name);

/* Read-only access to a target's data. */
JCE_API const JceMorphTarget *jce_morph_set_at(const JceMorphSet *set,
                                                uint32_t idx);

/* ── Mesh-pointer registry ─────────────────────────────────────── *
 *
 * Bind a morph set to a mesh pointer (typically `JceMesh *`) so
 * runtime systems can fetch it via `find_for_mesh`.  Caller chooses
 * the keying — any stable pointer-sized identifier works.  Each key
 * can have at most one set; replacing an existing binding destroys
 * the old set. */
JCE_API bool         jce_morph_attach_to_mesh(const void *mesh_key,
                                              JceMorphSet *set);
JCE_API JceMorphSet *jce_morph_find_for_mesh(const void *mesh_key);
JCE_API void         jce_morph_detach_from_mesh(const void *mesh_key);

/* Apply a weighted blend of targets into out_positions / out_normals
 * given a base pose and per-target weights.  Pure-CPU; useful for
 * preview, server-side shape baking, or fallback when GPU morph isn't
 * available.  out arrays must each be `vertex_count*3` long. */
JCE_API void jce_morph_apply_cpu(const JceMorphSet *set,
                                 const float       *base_positions,
                                 const float       *base_normals, /* may be NULL */
                                 const float       *weights,      /* one per target */
                                 float             *out_positions,
                                 float             *out_normals); /* may be NULL */

JCE_EXTERN_C_END

#endif /* JCE_MORPH_TARGET_H */
