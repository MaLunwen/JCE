/*
 * jce_gltf_loader.h  glTF 2.0 / GLB model loading from PAK.
 */
#ifndef JCE_GLTF_LOADER_H
#define JCE_GLTF_LOADER_H

#include "graphics/jce_model.h"

typedef struct PakArchive PakArchive;

/* Load a glTF/GLB model from PAK archive.
 * Extracts meshes, PBR materials, textures, skeleton, and animations.
 * Returns NULL on failure. */
JceModel *jce_gltf_load(const PakArchive *pak, const char *asset_path);

#endif /* JCE_GLTF_LOADER_H */
