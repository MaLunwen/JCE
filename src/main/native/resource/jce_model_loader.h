/*
 * jce_model_loader.h  Model loading from PAK via assimp (C API).
 */

#ifndef JCE_MODEL_LOADER_H
#define JCE_MODEL_LOADER_H

#include "graphics/jce_mesh.h"

typedef struct PakArchive PakArchive;

/* Load the first mesh from a model file in PAK (e.g. "models/chalet.obj").
   Returns NULL on failure. */
JceMesh *jce_model_load(const PakArchive *pak, const char *asset_path);

#endif /* JCE_MODEL_LOADER_H */
