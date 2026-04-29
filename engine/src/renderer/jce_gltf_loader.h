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

#endif /* JCE_GLTF_LOADER_H */
