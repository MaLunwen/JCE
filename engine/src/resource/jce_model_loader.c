/*
 * jce_model_loader.c  Model loading stub.
 *
 * The assimp-based implementation has been moved to the editor module.
 * The engine uses cgltf for glTF loading (jce_gltf_loader / jce_cgltf_loader).
 * This stub returns NULL for non-glTF formats.
 */

#include "jce_model_loader.h"
#include <jce/core/jce_log.h>
#include <stddef.h>

#define LOG_TAG "jce_model"

JceMesh *jce_model_load(const PakArchive *pak, const char *asset_path)
{
    (void)pak; (void)asset_path;
    LOG_WARN(LOG_TAG, "assimp not available in engine; use jce_model_load_gltf() for glTF assets");
    return NULL;
}
