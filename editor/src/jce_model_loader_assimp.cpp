/*
 * jce_model_loader_assimp.cpp  Model loading via assimp (editor-only).
 *
 * Uses assimp's C++ interface to load models from PAK assets.
 * Supports all assimp-supported formats: OBJ, FBX, 3DS, glTF, etc.
 */

#include "jce_model_loader_assimp.h"

extern "C" {
#include <jce/core/jce_log.h>
}

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <SDL3/SDL.h>
#include <cstring>

#define LOG_TAG "editor_model"

extern "C" {

void jce_editor_model_free_cpu_data(JceEditorCpuMeshData *data)
{
    if (!data) return;
    SDL_free(data->vertices);
    SDL_free(data->indices);
    data->vertices = nullptr;
    data->indices = nullptr;
    data->vertex_count = 0;
    data->index_count = 0;
}

static bool build_cpu_mesh_data(const aiScene *scene, JceEditorCpuMeshData *out)
{
    if (!scene || scene->mNumMeshes == 0 || !out) return false;

    out->vertices = nullptr;
    out->indices = nullptr;
    out->vertex_count = 0;
    out->index_count = 0;

    uint32_t total_verts = 0;
    uint32_t total_indices = 0;
    for (unsigned m = 0; m < scene->mNumMeshes; m++) {
        const aiMesh *ai = scene->mMeshes[m];
        total_verts += ai->mNumVertices;
        for (unsigned f = 0; f < ai->mNumFaces; f++) {
            if (ai->mFaces[f].mNumIndices == 3)
                total_indices += 3;
        }
    }

    if (total_verts == 0) return false;

    auto *verts = static_cast<JceMeshVertex *>(
        SDL_calloc(total_verts, sizeof(JceMeshVertex)));
    auto *indices = (total_indices > 0)
        ? static_cast<uint32_t *>(SDL_malloc(
            static_cast<size_t>(total_indices) * sizeof(uint32_t)))
        : nullptr;
    if (!verts || (total_indices > 0 && !indices)) {
        SDL_free(verts);
        SDL_free(indices);
        return false;
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

    out->vertices = verts;
    out->indices = indices;
    out->vertex_count = total_verts;
    out->index_count = idx_count;
    return true;
}

/* Merge all meshes in the scene into a single JceMesh. */
static JceMesh *convert_all_meshes(const aiScene *scene)
{
    JceEditorCpuMeshData cpu = {};
    if (!build_cpu_mesh_data(scene, &cpu))
        return nullptr;

    JceMesh *mesh = jce_mesh_create(cpu.vertices, cpu.vertex_count,
                                    cpu.indices, cpu.index_count);
    jce_editor_model_free_cpu_data(&cpu);
    return mesh;
}

bool jce_editor_model_load_cpu_file(const char *file_path,
                                    JceEditorCpuMeshData *out)
{
    if (!file_path || file_path[0] == '\0' || !out) return false;

    out->vertices = nullptr;
    out->indices = nullptr;
    out->vertex_count = 0;
    out->index_count = 0;

    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(
        file_path,
        aiProcess_Triangulate
        | aiProcess_GenSmoothNormals
        | aiProcess_FlipUVs
        | aiProcess_CalcTangentSpace);

    if (!scene || !scene->mNumMeshes) {
        LOG_ERROR(LOG_TAG, "assimp cpu file load failed: %s  %s",
                  file_path, importer.GetErrorString());
        return false;
    }

    if (!build_cpu_mesh_data(scene, out)) {
        LOG_ERROR(LOG_TAG, "cpu mesh conversion failed: %s", file_path);
        return false;
    }

    LOG_DEBUG(LOG_TAG, "decoded cpu mesh %s (%u verts, %u tris)",
              file_path, out->vertex_count, out->index_count / 3);
    return true;
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
