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
#include "jce_mesh_lod_cook.h"
}

#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/material.h>

#include <string>

#include <meshoptimizer.h>

#include <cstring>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
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

/* AUTO-LOD GENERATION (additive, cook-time).
 *
 * After the dedup/cache-optimise pass the mesh's positions + index buffer are
 * final.  Drive the meshoptimizer-backed core (jce_mesh_generate_lod_chain)
 * over the mesh's position stream to produce K simplified index buffers, then
 * log the per-level triangle reduction (mirroring the existing pre/post-meshopt
 * logging).  Returns the number of LOD levels generated for this mesh.
 *
 * The generated index buffers are validated and accounted for here; persisting
 * them into the shipped asset is the documented storage followup (the bundle
 * mesh path exports a single .glb blob via Assimp, which has no .jceasset chunk
 * slot — the JCEASSET_CHUNK_MESH_LOD_INDICES chunk type + JceAssetMeshLodHeader
 * are registered so the sidecar/.jceasset store and runtime consumption can be
 * wired without a format change).  Generation is additive: it never mutates the
 * mesh, so the exported base (LOD0) GLB is byte-identical to today. */
static size_t generate_mesh_lods(const aiMesh *mesh)
{
    if (!mesh || !mesh->HasPositions()) return 0;
    if (mesh->mNumVertices == 0 || mesh->mNumFaces == 0) return 0;

    const size_t vn = mesh->mNumVertices;
    const size_t in = (size_t)mesh->mNumFaces * 3;
    if (in < 3) return 0;

    /* Tightly packed float[3] position stream for the simplifier. */
    std::vector<float> positions(vn * 3);
    for (size_t i = 0; i < vn; ++i) {
        positions[i * 3 + 0] = mesh->mVertices[i].x;
        positions[i * 3 + 1] = mesh->mVertices[i].y;
        positions[i * 3 + 2] = mesh->mVertices[i].z;
    }

    std::vector<unsigned int> base_indices(in);
    for (size_t f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        if (face.mNumIndices != 3) return 0; /* must be triangulated */
        base_indices[f * 3 + 0] = face.mIndices[0];
        base_indices[f * 3 + 1] = face.mIndices[1];
        base_indices[f * 3 + 2] = face.mIndices[2];
    }

    unsigned int *lod_indices[JCE_MESH_LOD_MAX_LEVELS] = { nullptr };
    size_t        lod_counts[JCE_MESH_LOD_MAX_LEVELS]  = { 0 };

    const size_t levels = jce_mesh_generate_lod_chain(
        positions.data(), vn, 3 * sizeof(float),
        base_indices.data(), in,
        JCE_MESH_LOD_DEFAULT_RATIOS, JCE_MESH_LOD_DEFAULT_LEVEL_COUNT,
        lod_indices, lod_counts);

    const size_t base_tris = in / 3;
    for (size_t l = 0; l < levels; ++l) {
        const size_t tris = lod_counts[l] / 3;
        LOG_INFO(LOG_TAG, "meshopt LOD '%s' level %zu: %zu -> %zu tris (%.0f%%)",
                 mesh->mName.C_Str(), l + 1, base_tris, tris,
                 base_tris ? (100.0 * (double)tris / (double)base_tris) : 0.0);
        JCE_FREE(lod_indices[l]);   /* caller owns each level; freed here */
    }

    return levels;
}

/* ---- Indexed-GLB writer ---------------------------------------------------
 * Assimp 6's glTF2 exporter writes UN-INDEXED triangle soup (a clean 265K-vert
 * indexed mesh comes out as 1.5M verts == 1.5M identity indices), inflating the
 * shipped bundle 2-6x.  We bypass it: the meshopt pass above already leaves
 * each aiMesh welded + indexed, so we serialise that directly as an indexed
 * binary glTF.  Geometry (POSITION/NORMAL/TEXCOORD_0 + indices) + basic
 * baseColorFactor materials only.  Returns false (caller falls back to Assimp's
 * exporter) when the scene carries textures/embedded images, so textured models
 * keep their full material export. */

static void jw_appendf(std::string &s, const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) s.append(buf, (size_t)((n < (int)sizeof buf) ? n : (int)sizeof buf - 1));
}

static void jw_align4(std::vector<uint8_t> &b) { while (b.size() & 3u) b.push_back(0); }

static void jw_put_bytes(std::vector<uint8_t> &b, const void *p, size_t n)
{
    const uint8_t *u = static_cast<const uint8_t *>(p);
    b.insert(b.end(), u, u + n);
}

static bool write_indexed_glb(const aiScene *scene, uint8_t **out_buf, size_t *out_size)
{
    if (!scene || scene->mNumMeshes == 0) return false;
    if (scene->mNumTextures > 0) return false;          /* embedded images: keep assimp path */
    for (unsigned i = 0; i < scene->mNumMaterials; ++i) {
        const aiMaterial *m = scene->mMaterials[i];
        if (m->GetTextureCount(aiTextureType_DIFFUSE) > 0 ||
            m->GetTextureCount(aiTextureType_BASE_COLOR) > 0)
            return false;                               /* textured: keep assimp path */
    }

    std::vector<uint8_t> bin;
    std::string jbv, jacc, jmesh, jnode, jscene;
    int bv = 0, ac = 0, node = 0;

    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh *me = scene->mMeshes[mi];
        if (!me->HasPositions() || me->mNumVertices == 0 || me->mNumFaces == 0)
            continue;
        const bool     hn = me->HasNormals();
        const bool     hu = me->HasTextureCoords(0);
        const uint32_t V  = me->mNumVertices;

        std::vector<uint32_t> idx;
        idx.reserve((size_t)me->mNumFaces * 3);
        for (unsigned f = 0; f < me->mNumFaces; ++f) {
            const aiFace &fc = me->mFaces[f];
            if (fc.mNumIndices != 3) return false;      /* must be triangulated */
            idx.push_back(fc.mIndices[0]);
            idx.push_back(fc.mIndices[1]);
            idx.push_back(fc.mIndices[2]);
        }
        if (idx.empty()) continue;

        /* POSITION (with required min/max) */
        jw_align4(bin);
        size_t pos_off = bin.size();
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        for (uint32_t v = 0; v < V; ++v) {
            float p[3] = { me->mVertices[v].x, me->mVertices[v].y, me->mVertices[v].z };
            for (int k = 0; k < 3; ++k) { if (p[k] < mn[k]) mn[k] = p[k]; if (p[k] > mx[k]) mx[k] = p[k]; }
            jw_put_bytes(bin, p, 12);
        }
        if (!jbv.empty()) jbv += ',';
        jw_appendf(jbv, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%u,\"target\":34962}", pos_off, V * 12u);
        int pos_bv = bv++;
        if (!jacc.empty()) jacc += ',';
        jw_appendf(jacc, "{\"bufferView\":%d,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\","
                   "\"min\":[%.9g,%.9g,%.9g],\"max\":[%.9g,%.9g,%.9g]}",
                   pos_bv, V, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
        int pos_ac = ac++;

        int nrm_ac = -1, uv_ac = -1;
        if (hn) {
            jw_align4(bin); size_t off = bin.size();
            for (uint32_t v = 0; v < V; ++v) { float p[3] = { me->mNormals[v].x, me->mNormals[v].y, me->mNormals[v].z }; jw_put_bytes(bin, p, 12); }
            jbv += ','; jw_appendf(jbv, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%u,\"target\":34962}", off, V * 12u);
            int b_ = bv++; jacc += ','; jw_appendf(jacc, "{\"bufferView\":%d,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"}", b_, V);
            nrm_ac = ac++;
        }
        if (hu) {
            jw_align4(bin); size_t off = bin.size();
            for (uint32_t v = 0; v < V; ++v) { float p[2] = { me->mTextureCoords[0][v].x, me->mTextureCoords[0][v].y }; jw_put_bytes(bin, p, 8); }
            jbv += ','; jw_appendf(jbv, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%u,\"target\":34962}", off, V * 8u);
            int b_ = bv++; jacc += ','; jw_appendf(jacc, "{\"bufferView\":%d,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"}", b_, V);
            uv_ac = ac++;
        }

        /* indices */
        jw_align4(bin); size_t ioff = bin.size();
        jw_put_bytes(bin, idx.data(), idx.size() * 4);
        jbv += ','; jw_appendf(jbv, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963}", ioff, idx.size() * 4);
        int idx_bv = bv++; jacc += ','; jw_appendf(jacc, "{\"bufferView\":%d,\"componentType\":5125,\"count\":%zu,\"type\":\"SCALAR\"}", idx_bv, idx.size());
        int idx_ac = ac++;

        if (!jmesh.empty()) jmesh += ',';
        jw_appendf(jmesh, "{\"primitives\":[{\"attributes\":{\"POSITION\":%d", pos_ac);
        if (nrm_ac >= 0) jw_appendf(jmesh, ",\"NORMAL\":%d", nrm_ac);
        if (uv_ac  >= 0) jw_appendf(jmesh, ",\"TEXCOORD_0\":%d", uv_ac);
        jw_appendf(jmesh, "},\"indices\":%d,\"material\":%u,\"mode\":4}]}", idx_ac, me->mMaterialIndex);

        if (!jnode.empty())  jnode  += ',';
        if (!jscene.empty()) jscene += ',';
        jw_appendf(jnode, "{\"mesh\":%d}", node);
        jw_appendf(jscene, "%d", node);
        node++;
    }
    if (node == 0) return false;

    std::string jmat;
    for (unsigned i = 0; i < scene->mNumMaterials; ++i) {
        aiColor4D kd(1.f, 1.f, 1.f, 1.f);
        aiGetMaterialColor(scene->mMaterials[i], AI_MATKEY_COLOR_DIFFUSE, &kd);
        if (!jmat.empty()) jmat += ',';
        jw_appendf(jmat, "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[%.6g,%.6g,%.6g,%.6g],"
                   "\"metallicFactor\":0.0,\"roughnessFactor\":1.0}}", kd.r, kd.g, kd.b, kd.a);
    }

    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"jce-indexed-glb\"},";
    jw_appendf(json, "\"buffers\":[{\"byteLength\":%zu}],", bin.size());
    json += "\"bufferViews\":[" + jbv + "],";
    json += "\"accessors\":[" + jacc + "],";
    if (!jmat.empty()) json += "\"materials\":[" + jmat + "],";
    json += "\"meshes\":[" + jmesh + "],";
    json += "\"nodes\":[" + jnode + "],";
    json += "\"scenes\":[{\"nodes\":[" + jscene + "]}],\"scene\":0}";

    while (json.size() & 3u) json += ' ';   /* JSON chunk: pad with spaces */
    jw_align4(bin);                          /* BIN chunk: pad with zeros  */

    const uint32_t jlen = (uint32_t)json.size();
    const uint32_t blen = (uint32_t)bin.size();
    const uint32_t total = 12u + 8u + jlen + 8u + blen;
    uint8_t *out = static_cast<uint8_t *>(JCE_MALLOC(total));
    if (!out) return false;
    uint8_t *w = out;
    auto put32 = [&](uint32_t x) { std::memcpy(w, &x, 4); w += 4; };
    put32(0x46546C67u);   /* "glTF" */
    put32(2u);
    put32(total);
    put32(jlen); put32(0x4E4F534Au /* "JSON" */); std::memcpy(w, json.data(), jlen); w += jlen;
    put32(blen); put32(0x004E4942u /* "BIN\0" */); if (blen) std::memcpy(w, bin.data(), blen); w += blen;

    *out_buf = out;
    *out_size = total;
    LOG_INFO(LOG_TAG, "indexed-glb writer: %d mesh(es), %u bytes (bypassed assimp soup exporter)",
             node, total);
    return true;
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
        /* MUST match the editor's mesh importer (jce_model_importer.cpp), which
         * applies aiProcess_FlipUVs to bring OBJ/FBX (V-up origin) into the
         * engine's top-left (V-down) texture convention.  Without it a converted
         * model's UVs sample a different atlas region than the editor rendered
         * the source with -> wrong colors. */
        | aiProcess_FlipUVs
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

    /* AUTO-LOD GENERATION (additive): drive the meshopt simplify core over
     * each finalised mesh.  Generation does not mutate the scene, so the
     * exported base GLB is byte-identical to before. */
    {
        size_t total_lods = 0;
        for (unsigned i = 0; i < scene->mNumMeshes; ++i)
            total_lods += generate_mesh_lods(scene->mMeshes[i]);
        if (total_lods > 0)
            LOG_INFO(LOG_TAG, "auto-LOD: generated %zu LOD level(s) across %u mesh(es)",
                     total_lods, scene->mNumMeshes);
    }

    /* Preferred path: write an INDEXED glb ourselves (Assimp's glTF2 exporter
     * un-indexes to triangle soup, inflating the bundle 2-6x).  Falls back to
     * the Assimp exporter for textured / embedded-image scenes. */
    if (write_indexed_glb(scene, out_buf, out_size))
        return 1;

    /* Fallback: Assimp's validated glb2 exporter (binary glTF 2.0). */
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
