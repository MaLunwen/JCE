/*
 * jce_gltf_loader.h  glTF 2.0 / GLB model loading from PAK.
 */
#ifndef JCE_GLTF_LOADER_H
#define JCE_GLTF_LOADER_H

#include "jce_model.h"
#include <jce/renderer/jce_skinned_mesh.h>   /* JceSkinnedVertex / JcePbrVertex */
#include <stdbool.h>
#include <stdint.h>

typedef struct JcePakArchive JcePakArchive;

/* Load a glTF/GLB model from PAK archive.
 * Extracts meshes, PBR materials, textures, skeleton, and animations.
 * Returns NULL on failure. */
JceModel *jce_gltf_load(const JcePakArchive *pak, const char *asset_path);
JceModel *jce_gltf_load_memory(const void *data, uint32_t size,
                               const char *name);

/* Header-only rig probe: cgltf-parses just the JSON header (no buffer
 * load, image decode, mesh build, or animation sampling) to report
 * whether the model carries a skin and/or animation clips.  Cheap enough
 * to call on the main thread.  Returns false on parse failure. */
bool jce_gltf_probe_rig_memory(const void *data, uint32_t size,
                               bool *out_has_skin, bool *out_has_anim);

/* ── Worker-decode + render-thread-upload split ───────────────────────
 *
 * jce_gltf_load() does cgltf parse + CPU extraction AND bgfx mesh/texture
 * creation in one call.  This pair separates them so the slow, GPU-free
 * part (parse + vertex extraction + image decode) runs on a worker and
 * only the cheap GPU resource creation happens on the render thread:
 *
 *   worker:        JceModelCpu *c = jce_gltf_decode_cpu(pak, path);
 *   render thread: JceModel    *m = jce_gltf_upload_cpu(c);   // consumes c
 *
 * decode_cpu touches no bgfx (meshes stay as CPU vertex/index arrays,
 * textures as decoded CPU pixels).  upload_cpu creates the bgfx buffers
 * + textures on the calling thread and frees `c`.  jce_gltf_model_cpu_free
 * releases a decoded result without upload (cancellation). */
typedef struct JceModelCpu JceModelCpu;

JceModelCpu *jce_gltf_decode_cpu(const JcePakArchive *pak, const char *asset_path);
JceModelCpu *jce_gltf_decode_cpu_memory(const void *data, uint32_t size,
                                        const char *name);
JceModel    *jce_gltf_upload_cpu(JceModelCpu *cpu);
void         jce_gltf_model_cpu_free(JceModelCpu *cpu);

/* ── Morph-target inspection on the CPU intermediate (FEATURE 3.1) ─────
 *
 * These read the decoded-but-not-uploaded JceModelCpu so the morph import can
 * be exercised without a bgfx context (the upload step is what creates GPU
 * resources).  Used by the runtime's morph-weight wiring and by unit tests. */
struct JceMorphData;
struct JceMorphWeightTrack;

uint32_t jce_gltf_cpu_node_count(const JceModelCpu *cpu);
uint32_t jce_gltf_cpu_node_prim_count(const JceModelCpu *cpu, uint32_t node);

/* Morph deltas for one primitive (node, prim), or NULL if it has none. */
const struct JceMorphData *jce_gltf_cpu_prim_morph(const JceModelCpu *cpu,
                                                   uint32_t node, uint32_t prim);

/* Imported morph-weight animation tracks. */
uint32_t jce_gltf_cpu_morph_anim_count(const JceModelCpu *cpu);
const struct JceMorphWeightTrack *jce_gltf_cpu_morph_anim_track(
    const JceModelCpu *cpu, uint32_t index,
    uint32_t *out_anim_index, uint32_t *out_node_index);

/* ── Generic CPU-model builder (format-agnostic skinned import) ────────
 *
 * The struct JceModelCpu intermediate is renderer-private; this builder lets an
 * importer in another layer (e.g. the assimp FBX path in jce_resource) hand over
 * already-extracted CPU geometry/skeleton/clips so the SAME jce_gltf_upload_cpu
 * GPU-upload path can be reused.  All builder calls are bgfx-free (CPU arrays +
 * the animation layer's CPU skeleton/clip creators), so a JceModelCpu can be
 * built and inspected via the jce_gltf_cpu_* accessors WITHOUT a GPU context
 * (headless tests), then uploaded later with jce_gltf_upload_cpu() on the
 * render thread.  Vertex/index arrays are COPIED; skeleton/clip ownership MOVES
 * into the builder.  Release an un-uploaded result with jce_gltf_model_cpu_free.
 */
JceModelCpu *jce_model_cpu_builder_create(void);
bool jce_model_cpu_builder_reserve_nodes(JceModelCpu *cpu, uint32_t num_nodes);
bool jce_model_cpu_builder_set_skinned_node(JceModelCpu *cpu, uint32_t node_index,
                                            const char *name,
                                            const jce_mat4 *local_transform,
                                            const JceSkinnedVertex *verts,
                                            uint32_t num_verts,
                                            const uint32_t *indices,
                                            uint32_t num_indices,
                                            uint32_t material_index);
bool jce_model_cpu_builder_set_static_node(JceModelCpu *cpu, uint32_t node_index,
                                           const char *name,
                                           const jce_mat4 *local_transform,
                                           const JcePbrVertex *verts,
                                           uint32_t num_verts,
                                           const uint32_t *indices,
                                           uint32_t num_indices,
                                           uint32_t material_index);
void jce_model_cpu_builder_set_skeleton(JceModelCpu *cpu, JceSkeleton *skel);
void jce_model_cpu_builder_set_anims(JceModelCpu *cpu,
                                     JceAnimClip **clips, uint32_t count);
bool jce_model_cpu_builder_set_default_material(JceModelCpu *cpu);

/* ── Skeleton / skin / anim inspection on the CPU intermediate ─────────
 * Read a decoded-but-not-uploaded JceModelCpu (no GPU context needed) so the
 * skinned-import extraction can be asserted headless. */
const JceSkeleton *jce_gltf_cpu_skeleton(const JceModelCpu *cpu);
uint32_t           jce_gltf_cpu_anim_count(const JceModelCpu *cpu);
const JceAnimClip *jce_gltf_cpu_anim_clip(const JceModelCpu *cpu, uint32_t index);
bool     jce_gltf_cpu_prim_is_skinned(const JceModelCpu *cpu,
                                      uint32_t node, uint32_t prim);
uint32_t jce_gltf_cpu_prim_vertex_count(const JceModelCpu *cpu,
                                        uint32_t node, uint32_t prim);
bool     jce_gltf_cpu_prim_skinned_weights(const JceModelCpu *cpu,
                                           uint32_t node, uint32_t prim,
                                           uint32_t vtx, float out_w[4]);

#endif /* JCE_GLTF_LOADER_H */
