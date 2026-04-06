/*
 * jce_model_loader_assimp.cpp  Model loading via assimp (editor-only).
 *
 * Uses assimp's C++ interface to load models from PAK assets.
 * Supports all assimp-supported formats: OBJ, FBX, 3DS, glTF, etc.
 */

extern "C" {
#include <jce/graphics/jce_mesh.h>
#include <jce/resource/pak_loader.h>
#include <jce/core/jce_log.h>
}

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <SDL3/SDL.h>
#include <cstring>

#define LOG_TAG "editor_model"

extern "C" {

/* Merge all meshes in the scene into a single JceMesh. */
static JceMesh *convert_all_meshes(const aiScene *scene)
{
    if (!scene || scene->mNumMeshes == 0) return nullptr;

    /* First pass: count totals. */
    uint32_t total_verts = 0;
    uint32_t total_indices = 0;
    for (unsigned m = 0; m < scene->mNumMeshes; m++) {
        const aiMesh *ai = scene->mMeshes[m];
        total_verts += ai->mNumVertices;
        for (unsigned f = 0; f < ai->mNumFaces; f++)
            if (ai->mFaces[f].mNumIndices == 3)
                total_indices += 3;
    }

    if (total_verts == 0) return nullptr;

    auto *verts = static_cast<JceMeshVertex *>(
        SDL_calloc(total_verts, sizeof(JceMeshVertex)));
    auto *indices = static_cast<uint32_t *>(
        SDL_malloc(static_cast<size_t>(total_indices) * sizeof(uint32_t)));
    if (!verts || !indices) {
        SDL_free(verts);
        SDL_free(indices);
        return nullptr;
    }

    uint32_t idx_count = 0;
    uint32_t vertex_offset = 0;
    for (unsigned m = 0; m < scene->mNumMeshes; m++) {
        const aiMesh *ai = scene->mMeshes[m];

        for (unsigned i = 0; i < ai->mNumVertices; i++) {
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

        for (unsigned f = 0; f < ai->mNumFaces; f++) {
            const aiFace &face = ai->mFaces[f];
            if (face.mNumIndices != 3) continue;
            for (unsigned j = 0; j < 3; j++)
                indices[idx_count++] = face.mIndices[j] + vertex_offset;
        }

        vertex_offset += ai->mNumVertices;
    }

    JceMesh *mesh = jce_mesh_create(verts, total_verts, indices, idx_count);
    SDL_free(verts);
    SDL_free(indices);
    return mesh;
}

JceMesh *jce_editor_model_load(const PakArchive *pak, const char *asset_path)
{
    if (!pak || !asset_path) return nullptr;

    const PakAsset *asset = pak_find(pak, asset_path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "model not found in PAK: %s", asset_path);
        return nullptr;
    }

    void *buf = SDL_malloc(static_cast<size_t>(asset->original_size));
    if (!buf) return nullptr;

    size_t n = pak_decompress(asset, buf, static_cast<size_t>(asset->original_size));
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", asset_path);
        SDL_free(buf);
        return nullptr;
    }

    /* Determine file extension hint for assimp. */
    const char *ext = "";
    const char *dot = strrchr(asset_path, '.');
    if (dot) ext = dot;

    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFileFromMemory(
        buf, n,
        aiProcess_Triangulate
        | aiProcess_GenSmoothNormals
        | aiProcess_FlipUVs
        | aiProcess_CalcTangentSpace,
        ext);

    SDL_free(buf);

    if (!scene || !scene->mNumMeshes) {
        LOG_ERROR(LOG_TAG, "assimp failed: %s  %s",
                  asset_path, importer.GetErrorString());
        return nullptr;
    }

    JceMesh *mesh = convert_all_meshes(scene);
    if (mesh) {
        LOG_DEBUG(LOG_TAG, "loaded %s (%u meshes merged, %u verts, %u tris)",
                  asset_path, scene->mNumMeshes,
                  jce_mesh_vertex_count(mesh),
                  jce_mesh_index_count(mesh) / 3);
    }

    return mesh;
}

JceMesh *jce_editor_model_load_file(const char *file_path)
{
    if (!file_path || file_path[0] == '\0') return nullptr;

    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(
        file_path,
        aiProcess_Triangulate
        | aiProcess_GenSmoothNormals
        | aiProcess_FlipUVs
        | aiProcess_CalcTangentSpace);

    if (!scene || !scene->mNumMeshes) {
        LOG_ERROR(LOG_TAG, "assimp file load failed: %s  %s",
                  file_path, importer.GetErrorString());
        return nullptr;
    }

    JceMesh *mesh = convert_all_meshes(scene);
    if (mesh) {
        LOG_DEBUG(LOG_TAG, "loaded file %s (%u meshes merged, %u verts, %u tris)",
                  file_path, scene->mNumMeshes,
                  jce_mesh_vertex_count(mesh),
                  jce_mesh_index_count(mesh) / 3);
    }

    return mesh;
}

} /* extern "C" */
