/*
 * jce_bundle_mesh_convert.cpp  Bundle-time mesh-to-glb conversion.
 *
 * The bundle packer calls into this TU to coerce any Assimp-readable
 * mesh format (OBJ, FBX, DAE, 3DS, PLY, STL, …) into a self-contained
 * binary glTF (.glb) blob.  The output keeps geometry + materials so
 * the runtime can mount a single mesh loader (cgltf) and never has to
 * see the source format.
 *
 * C ABI only — declared via extern "C" so jce_bundle_pack.c (C99) can
 * call straight in without dragging Assimp headers across the layer.
 *
 * Buffer ownership: the returned blob is JCE_MALLOC'd; the caller
 * releases with JCE_FREE.
 */

#include <jce/os/core/jce_defs.h>

extern "C" {
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_log.h>
}

#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <meshoptimizer.h>

#include <cstring>
#include <cstdint>
#include <cstddef>
#include <vector>

#define LOG_TAG "mesh_convert"

/* ---- C++ helpers (NOT extern "C") ------------------------------------ */

/* Dedup vertices on `mesh` in place using meshoptimizer.
 *
 * Assimp's `aiProcess_GenSmoothNormals` splits shared vertices at hard
 * edges, which can inflate an OBJ with 265 K shared corners to 1.5 M
 * un-shared face-corners.  We rebuild the mesh by keying the remap on
 * *position + UV only* (normals are excluded because they are the thing
 * causing the split), then average the per-corner normals into the new
 * merged vertices so the smooth shading is still correct.
 *
 * Only the common renderable attributes are handled: positions, normals
 * (if present), and UV0 (if present).  Meshes carrying other attributes
 * (vertex colours, bones, multiple UV sets, …) are skipped so we never
 * silently drop data. */
static void remap_mesh_vertices(aiMesh *mesh) {
    if (!mesh) return;
    if (!mesh->HasPositions()) return;
    if (mesh->mNumVertices == 0 || mesh->mNumFaces == 0) return;
    /* Bail on attributes we don't repack so we never lose data. */
    if (mesh->HasVertexColors(0)) {
        LOG_INFO(LOG_TAG, "meshopt skip '%s': has vertex colors", mesh->mName.C_Str());
        return;
    }
    if (mesh->HasBones()) {
        LOG_INFO(LOG_TAG, "meshopt skip '%s': has bones", mesh->mName.C_Str());
        return;
    }
    if (mesh->GetNumUVChannels() > 1) {
        LOG_INFO(LOG_TAG, "meshopt skip '%s': %u UV channels", mesh->mName.C_Str(), mesh->GetNumUVChannels());
        return;
    }

    const bool has_nrm = mesh->HasNormals();
    const bool has_uv  = mesh->HasTextureCoords(0);

    /* Hash key: position + UV only.  Normals are excluded so that
     * vertices at hard edges (same pos + uv, different smooth normal)
     * still get merged.  We re-derive normals as a weighted average
     * after the merge so shading remains correct. */
    struct Key { float pos[3], uv[2]; };
    const size_t vn = mesh->mNumVertices;
    const size_t in = (size_t)mesh->mNumFaces * 3;

    std::vector<Key>          keys(vn);
    std::vector<unsigned int> indices(in);

    for (size_t i = 0; i < vn; ++i) {
        keys[i].pos[0] = mesh->mVertices[i].x;
        keys[i].pos[1] = mesh->mVertices[i].y;
        keys[i].pos[2] = mesh->mVertices[i].z;
        if (has_uv) {
            keys[i].uv[0] = mesh->mTextureCoords[0][i].x;
            keys[i].uv[1] = mesh->mTextureCoords[0][i].y;
        } else {
            keys[i].uv[0] = keys[i].uv[1] = 0.f;
        }
    }
    for (size_t f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        if (face.mNumIndices != 3) return; /* must be triangulated */
        indices[f * 3 + 0] = face.mIndices[0];
        indices[f * 3 + 1] = face.mIndices[1];
        indices[f * 3 + 2] = face.mIndices[2];
    }

    std::vector<unsigned int> remap(vn);
    const size_t new_vn = meshopt_generateVertexRemap(
        remap.data(), indices.data(), in,
        keys.data(), vn, sizeof(Key));

    const bool did_dedup = (new_vn < vn);
    LOG_INFO(LOG_TAG, "meshopt '%s': %zu -> %zu verts (faces=%zu)%s",
             mesh->mName.C_Str(), vn, new_vn, (size_t)mesh->mNumFaces,
             did_dedup ? "" : " [no vertex dedup]");

    /* Remap and vertex-cache optimise — always beneficial, even when
     * vertex count did not change (reorders triangles for GPU cache). */
    std::vector<unsigned int> new_indices(in);
    meshopt_remapIndexBuffer(new_indices.data(), indices.data(), in, remap.data());
    meshopt_optimizeVertexCache(new_indices.data(), new_indices.data(), in, new_vn);

    if (did_dedup) {
        /* Rebuild vertex buffers with merged (deduped) vertices. */
        std::vector<aiVector3D> new_pos(new_vn);
        meshopt_remapVertexBuffer(new_pos.data(), mesh->mVertices, vn,
                                  sizeof(aiVector3D), remap.data());

        /* Remap UVs (direct copy — UV is part of the merge key, so the
         * first occurrence of any (pos, uv) pair defines the UV). */
        std::vector<aiVector3D> new_uv;
        if (has_uv) {
            new_uv.resize(new_vn);
            meshopt_remapVertexBuffer(new_uv.data(), mesh->mTextureCoords[0],
                                      vn, sizeof(aiVector3D), remap.data());
        }

        /* Average normals: accumulate all pre-merge normals into their
         * target slot, then normalise.  This reproduces smooth shading
         * correctly across the merged vertex set. */
        std::vector<aiVector3D> new_nrm(new_vn, aiVector3D(0.f, 0.f, 0.f));
        if (has_nrm) {
            for (size_t i = 0; i < vn; ++i) {
                const unsigned ni = remap[i];
                if (ni < (unsigned)new_vn)
                    new_nrm[ni] += mesh->mNormals[i];
            }
            for (size_t i = 0; i < new_vn; ++i)
                new_nrm[i].Normalize();
        }

        /* Replace mesh arrays.  Assimp allocates with `new[]`; we follow
         * the same convention so aiMesh::~aiMesh can safely `delete[]`. */
        delete[] mesh->mVertices;
        mesh->mVertices = new aiVector3D[new_vn];
        std::memcpy(mesh->mVertices, new_pos.data(), new_vn * sizeof(aiVector3D));

        if (has_nrm) {
            delete[] mesh->mNormals;
            mesh->mNormals = new aiVector3D[new_vn];
            std::memcpy(mesh->mNormals, new_nrm.data(), new_vn * sizeof(aiVector3D));
        }
        if (has_uv) {
            delete[] mesh->mTextureCoords[0];
            mesh->mTextureCoords[0] = new aiVector3D[new_vn];
            std::memcpy(mesh->mTextureCoords[0], new_uv.data(),
                        new_vn * sizeof(aiVector3D));
        }

        mesh->mNumVertices = (unsigned int)new_vn;
    }

    /* Write back the (cache-optimised) index buffer to Assimp faces.
     * When no dedup occurred the vertex buffer is unchanged; indices
     * still reference valid [0, vn) slots in their new triangle order. */
    for (size_t f = 0; f < mesh->mNumFaces; ++f) {
        aiFace &face = mesh->mFaces[f];
        face.mIndices[0] = new_indices[f * 3 + 0];
        face.mIndices[1] = new_indices[f * 3 + 1];
        face.mIndices[2] = new_indices[f * 3 + 2];
    }
}


/* Convert an in-memory mesh blob to a binary glTF (.glb) blob.
 *
 *   src/src_sz   - raw bytes of the source file (any Assimp format).
 *   ext_hint     - file extension WITHOUT the dot (e.g. "obj"), used
 *                  as Assimp's format hint.  May be NULL/empty.
 *   out_buf      - receives a freshly JCE_MALLOC'd buffer with the
 *                  .glb bytes.  Caller frees with JCE_FREE.
 *   out_size     - receives the size of *out_buf in bytes.
 *
 * Returns 1 on success, 0 on failure.  On failure *out_buf is NULL.
 */
extern "C" JCE_API int jce_bundle_convert_to_glb(const uint8_t *src,
                                      size_t         src_sz,
                                      const char    *ext_hint,
                                      uint8_t      **out_buf,
                                      size_t        *out_size)
{
    if (out_buf)  *out_buf  = nullptr;
    if (out_size) *out_size = 0;
    if (!src || src_sz == 0 || !out_buf || !out_size) return 0;

    Assimp::Importer importer;
    const unsigned int flags =
          aiProcess_Triangulate
        | aiProcess_JoinIdenticalVertices
        | aiProcess_GenSmoothNormals
        | aiProcess_SortByPType
        | aiProcess_RemoveRedundantMaterials
        | aiProcess_OptimizeMeshes
        | aiProcess_OptimizeGraph;

    const char *hint = (ext_hint && *ext_hint) ? ext_hint : "";
    const aiScene *scene =
        importer.ReadFileFromMemory(src, src_sz, flags, hint);
    if (!scene) {
        LOG_WARN(LOG_TAG, "assimp import failed: %s",
                 importer.GetErrorString());
        return 0;
    }

    /* meshopt dedup pass */
    {
        size_t total_verts = 0, total_faces = 0;
        for (unsigned i = 0; i < scene->mNumMeshes; ++i)
            total_verts += scene->mMeshes[i]->mNumVertices,
            total_faces += scene->mMeshes[i]->mNumFaces;
        LOG_INFO(LOG_TAG, "pre-meshopt: %u meshes, %zu verts, %zu faces",
                  scene->mNumMeshes, total_verts, total_faces);
    }
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        remap_mesh_vertices(scene->mMeshes[i]);
    }
    {
        size_t total_verts = 0, total_faces = 0;
        for (unsigned i = 0; i < scene->mNumMeshes; ++i)
            total_verts += scene->mMeshes[i]->mNumVertices,
            total_faces += scene->mMeshes[i]->mNumFaces;
        LOG_INFO(LOG_TAG, "post-meshopt: %u meshes, %zu verts, %zu faces",
                  scene->mNumMeshes, total_verts, total_faces);
    }

    /* Export via Assimp's validated glb2 exporter (binary glTF 2.0). */
    Assimp::Exporter exporter;
    const aiExportDataBlob *blob = exporter.ExportToBlob(scene, "glb2");
    if (!blob || blob->size == 0) {
        LOG_WARN(LOG_TAG, "assimp glb2 export failed: %s",
                 exporter.GetErrorString());
        return 0;
    }

    uint8_t *dst = static_cast<uint8_t *>(JCE_MALLOC(blob->size));
    if (!dst) return 0;
    std::memcpy(dst, blob->data, blob->size);
    *out_buf  = dst;
    *out_size = blob->size;
    return 1;
}
