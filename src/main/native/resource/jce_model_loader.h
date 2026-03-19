/*
 * jce_model_loader.h  Model loading from PAK via assimp (C API).
 */

#ifndef JCE_MODEL_LOADER_H
#define JCE_MODEL_LOADER_H

#include "renderer/jce_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PakArchive PakArchive;

/* Load the first mesh from a model file in PAK (e.g. "models/chalet.obj").
   Returns NULL on failure. */
JceMesh *jce_model_load(PakArchive *pak, const char *asset_path);

#ifdef __cplusplus
}
#endif

#endif /* JCE_MODEL_LOADER_H */
