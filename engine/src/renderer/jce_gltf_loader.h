/*
 * jce_gltf_loader.h  glTF 2.0 / GLB model loading from PAK.
 */
#ifndef JCE_GLTF_LOADER_H
#define JCE_GLTF_LOADER_H

#include "jce_model.h"

typedef struct JcePakArchive JcePakArchive;

/* Load a glTF/GLB model from PAK archive.
 * Extracts meshes, PBR materials, textures, skeleton, and animations.
 * Returns NULL on failure. */
JceModel *jce_gltf_load(const JcePakArchive *pak, const char *asset_path);
JceModel *jce_gltf_load_memory(const void *data, uint32_t size,
                               const char *name);

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

#endif /* JCE_GLTF_LOADER_H */
