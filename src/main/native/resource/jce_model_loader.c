/*
 * jce_model_loader.c  Model loading via assimp (C API).
 *
 * Uses assimp's C interface (cimport.h) so the entire engine can be
 * compiled as pure C.
 *
 * Define JCE_NO_ASSIMP to compile without assimp (returns NULL stub).
 */

#include "jce_model_loader.h"
#include "pak_loader.h"
#include "foundation/jce_log.h"

#define LOG_TAG "jce_model"

#ifndef JCE_NO_ASSIMP

#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <SDL3/SDL.h>

/* Merge all meshes in the scene into a single JceMesh. */
static JceMesh *convert_all_meshes(const struct aiScene *scene)
{
    unsigned m, f, i, j;

    if (!scene || scene->mNumMeshes == 0) return NULL;

    /* First pass: count totals. */
    uint32_t total_verts = 0;
    uint32_t total_indices = 0;
    for (m = 0; m < scene->mNumMeshes; m++) {
        const struct aiMesh *ai = scene->mMeshes[m];
        total_verts += ai->mNumVertices;
        for (f = 0; f < ai->mNumFaces; f++)
            if (ai->mFaces[f].mNumIndices == 3)
                total_indices += 3;
    }

    if (total_verts == 0) return NULL;

    JceMeshVertex *verts = (JceMeshVertex *)SDL_calloc(total_verts, sizeof(JceMeshVertex));
    uint32_t *indices = (uint32_t *)SDL_malloc((size_t)total_indices * sizeof(uint32_t));
    if (!verts || !indices) {
        SDL_free(verts);
        SDL_free(indices);
        return NULL;
    }

    uint32_t idx_count = 0;

    /* Second pass: copy data with index re-basing. */
    uint32_t vertex_offset = 0;
    for (m = 0; m < scene->mNumMeshes; m++) {
        const struct aiMesh *ai = scene->mMeshes[m];

        for (i = 0; i < ai->mNumVertices; i++) {
            JceMeshVertex *v = &verts[vertex_offset + i];
            v->pos[0] = ai->mVertices[i].x;
            v->pos[1] = ai->mVertices[i].y;
            v->pos[2] = ai->mVertices[i].z;

            if (ai->mNormals) {
                v->normal[0] = ai->mNormals[i].x;
                v->normal[1] = ai->mNormals[i].y;
                v->normal[2] = ai->mNormals[i].z;
            } else {
                v->normal[0] = 0.0f;
                v->normal[1] = 1.0f;
                v->normal[2] = 0.0f;
            }

            if (ai->mTextureCoords[0]) {
                v->uv[0] = ai->mTextureCoords[0][i].x;
                v->uv[1] = ai->mTextureCoords[0][i].y;
            } else {
                v->uv[0] = 0.0f;
                v->uv[1] = 0.0f;
            }
        }

        for (f = 0; f < ai->mNumFaces; f++) {
            const struct aiFace *face = &ai->mFaces[f];
            if (face->mNumIndices != 3) continue;
            for (j = 0; j < 3; j++)
                indices[idx_count++] = face->mIndices[j] + vertex_offset;
        }

        vertex_offset += ai->mNumVertices;
    }

    JceMesh *mesh = jce_mesh_create(verts, total_verts, indices, idx_count);
    SDL_free(verts);
    SDL_free(indices);
    return mesh;
}

JceMesh *jce_model_load(const PakArchive *pak, const char *asset_path)
{
    if (!pak || !asset_path) return NULL;

    const PakAsset *asset = pak_find(pak, asset_path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "model not found in PAK: %s", asset_path);
        return NULL;
    }

    void *buf = SDL_malloc((size_t)asset->original_size);
    if (!buf) return NULL;

    size_t n = pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", asset_path);
        SDL_free(buf);
        return NULL;
    }

    /* Determine file extension hint for assimp. */
    const char *ext = NULL;
    const char *dot = strrchr(asset_path, '.');
    if (dot) ext = dot; /* e.g. ".obj" */

    unsigned flags = aiProcess_Triangulate
                   | aiProcess_GenSmoothNormals
                   | aiProcess_FlipUVs
                   | aiProcess_CalcTangentSpace;

    const struct aiScene *scene = aiImportFileFromMemory(
        (const char *)buf, (unsigned int)n, flags, ext ? ext : "");

    SDL_free(buf);

    if (!scene || !scene->mNumMeshes) {
        LOG_ERROR(LOG_TAG, "assimp failed: %s  %s",
                  asset_path, aiGetErrorString());
        return NULL;
    }

    JceMesh *mesh = convert_all_meshes(scene);
    if (mesh) {
        LOG_DEBUG(LOG_TAG, "loaded %s (%u meshes merged, %u verts, %u tris)",
                  asset_path, scene->mNumMeshes,
                  jce_mesh_vertex_count(mesh),
                  jce_mesh_index_count(mesh) / 3);
    }

    aiReleaseImport(scene);
    return mesh;
}

#else /* JCE_NO_ASSIMP */

JceMesh *jce_model_load(const PakArchive *pak, const char *asset_path)
{
    (void)pak; (void)asset_path;
    LOG_WARN(LOG_TAG, "assimp not available  cannot load %s", asset_path);
    return NULL;
}

#endif /* JCE_NO_ASSIMP */
