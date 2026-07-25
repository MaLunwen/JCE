/*
 * jce_bundle_mesh_convert.cpp  Bundle-time mesh-to-glb conversion.
 *
 * The bundle packer calls into this TU to coerce any Assimp-readable
 * mesh format (OBJ, FBX, DAE, 3DS, PLY, STL, …) into a self-contained
 * binary glTF (.glb) blob.  The output keeps geometry + materials so
 * the runtime can mount a single mesh loader (cgltf) and never has to
 * see the source format.
 *
 * AUTHORITY BOUNDARY: this is the AUTHORING-format importer.  An input
 * that is ALREADY glTF belongs to cgltf (engine/src/renderer/jce_gltf_loader.c),
 * and pushing it back through here is a lossy round-trip: write_indexed_glb()
 * below emits POSITION/NORMAL/UV0 + a baseColorFactor material ONLY, so a
 * skin, an animation clip or a morph target that entered as glTF leaves as
 * bare geometry, and even the assimp glb2 exporter fallback re-encodes them
 * through assimp's own intermediate scene.  Callers must therefore decide
 * BEFORE calling: jce_bundle_pack.c probes such inputs with cgltf and ships
 * the rigged / morphed ones verbatim; the editor's import_ensure_glb likewise
 * passes .gltf/.glb through untouched.  The probe cannot live in this TU —
 * jce_cook compiles it without linking cgltf.
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

#include <algorithm>   /* std::sort / std::fill (DAG build) */
#include <cmath>       /* sqrtf (group-sphere merge)        */

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
 * over the mesh's position stream to produce K simplified index buffers.
 *
 * PERSISTENCE: the generated index buffers are CAPTURED into a MeshLods record
 * (one per scene mesh) so write_indexed_glb can serialise them INTO the shipped
 * .glb (the cooked mesh asset).  They are stored as additional SCALAR index
 * accessors that share the base mesh's POSITION vertex buffer, referenced from
 * a primitive-level `JCE_lod` glTF extension ({"indices":[acc,acc,...]}).  The
 * runtime loader (jce_gltf_loader.c) reads that extension and uploads one
 * alternate index buffer per LOD level (distant entities bind the reduced index
 * set, the high-detail vertex buffer is shared).  Embedding in the .glb keeps
 * the asset self-contained (one PAK entry, no sidecar lookup) — the .glb IS the
 * cooked asset, so this is the documented "persist into the cooked asset" path
 * (the registered JCEASSET_CHUNK_MESH_LOD_INDICES chunk targets the separate
 * .jceasset container, which the bundle mesh path does not produce).
 *
 * Determinism: meshopt_simplify / simplifySloppy are deterministic for a fixed
 * input, so the captured chain is byte-stable across re-cooks of the same source
 * (the per-asset XXH3 content hash therefore stays stable when the source does).
 *
 * Generation is additive: it never mutates the base mesh, so LOD0 geometry in
 * the exported GLB is byte-identical to before — only extra accessors/buffer
 * views + a primitive extension are appended (ignored by readers that don't
 * know JCE_lod). */

/* Captured LOD chain for one aiMesh (owns each level's index buffer). */
struct MeshLods {
    std::vector<std::vector<uint32_t>> levels;   /* levels[l] = LOD(l+1) indices */
};

static size_t generate_mesh_lods(const aiMesh *mesh, MeshLods *out)
{
    if (out) out->levels.clear();
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
        /* Capture this level into the per-mesh record (skip degenerate / a level
         * that did not actually reduce — a passthrough copy would just bloat the
         * asset and force a needless re-bind to identical geometry). */
        if (out && lod_indices[l] && lod_counts[l] >= 3 &&
            (lod_counts[l] % 3) == 0 && lod_counts[l] < in) {
            out->levels.emplace_back(lod_indices[l], lod_indices[l] + lod_counts[l]);
        }
        JCE_FREE(lod_indices[l]);   /* caller owns each level; freed here */
    }

    return levels;
}

/* MESHLET GENERATION (additive, cook-time — Nanite-lite V1 + V3 LOD DAG).
 *
 * For dense static meshes, split the index buffer into GPU-cullable clusters
 * with meshopt_buildMeshlets, then build a cluster-LOD DAG above the leaves
 * (group ~4 sibling clusters -> simplify the merged patch to ~50% with the
 * group's BOUNDARY VERTICES LOCKED -> re-split into ~2 coarser clusters,
 * recurse), and persist ALL levels as a `JCE_meshlets` primitive extension
 * ({"indices":A,"meshlets":B,"bounds":C,"errors":E} accessor ids):
 *   A = meshlet-grouped index buffer (GLOBAL vertex ids — flattened from the
 *       meshlet-local vertex/triangle tables so the runtime binds it directly
 *       against the base vertex buffer; leaves AND coarse clusters),
 *   B = 2 x u32 per meshlet  {index_offset, index_count} into A,
 *   C = 8 x f32 per meshlet  {sphere cx,cy,cz,r, cone axis xyz, cone cutoff}
 *       from meshopt_computeMeshletBounds (cone axis derives from FACE
 *       normals, so it is winding-consistent by construction — the runtime
 *       cull's cone backface test depends on that sign),
 *   E = 10 x f32 per meshlet {own_error, parent_error,
 *                             own-group sphere cx,cy,cz,r,
 *                             parent-group sphere cx,cy,cz,r} — errors in
 *       OBJECT units, own_error = max(simplification error, children's
 *       own_error) so it is MONOTONE up every DAG path; leaves carry
 *       {0, +BIG}.  The runtime cut (cs_meshlet_cull) draws a cluster iff
 *       own_error passes the screen-space tolerance measured at the OWN-
 *       group sphere and parent_error does not at the PARENT-group sphere.
 *       A child's parent-test and its parent's own-test therefore evaluate
 *       the IDENTICAL (error, sphere) pair — the two decisions are exactly
 *       complementary, so every surface region is drawn by exactly one
 *       ancestor: no holes, no double-draw (near parts fine, far parts
 *       coarse, within ONE mesh).  Boundary locking keeps group seams
 *       watertight; group spheres nest by construction (parent sphere
 *       bounds its members' spheres), keeping selection monotone in
 *       distance.
 * The runtime (jce_gltf_loader.c) uploads A as an alternate index buffer and
 * B+C+E as a COMPUTE_READ sidecar; cs_meshlet_cull cut+frustum+cone-culls
 * per CLUSTER and survivors draw via one submit_indirect (JCE_MESHLET_CULL).
 *
 * Like JCE_lod this is additive (base geometry byte-identical, unknown
 * readers ignore it) and deterministic for a fixed input.  Only meshes worth
 * a dispatch get one (>= JCE_MESHLET_COOK_MIN_TRIS); JCE_COOK_NO_MESHLETS=1
 * disables emission entirely, JCE_COOK_MESHLET_FLAT=1 emits leaves only
 * (V1 behaviour; the DAG roughly doubles the grouped-IB bytes). */

#define JCE_MESHLET_COOK_MIN_TRIS   4096u   /* below this a plain draw wins */
#define JCE_MESHLET_MAX_VERTS       64      /* meshopt-recommended defaults */
#define JCE_MESHLET_MAX_TRIS        124     /* (multiple of 4)              */
#define JCE_MESHLET_CONE_WEIGHT     0.5f    /* balance size vs cullability  */
#define JCE_MESHLET_DAG_GROUP       4       /* clusters merged per parent    */
#define JCE_MESHLET_DAG_MAX_LEVELS  14      /* safety cap on DAG depth       */
#define JCE_MESHLET_ROOT_ERROR      1.0e30f /* "no parent" sentinel          */

/* Captured meshlet set for one aiMesh (runtime-format arrays, see above). */
struct MeshMeshlets {
    std::vector<uint32_t> indices;   /* A: meshlet-grouped IB (global ids)    */
    std::vector<uint32_t> desc;      /* B: {index_offset, index_count} x N    */
    std::vector<float>    bounds;    /* C: {sphere xyzr, cone xyz cutoff} x N */
    std::vector<float>    errors;    /* E: 10 x N {own_err, parent_err,
                                      *   own-group sphere xyzr,
                                      *   parent-group sphere xyzr} (V3) */
};

/* One in-flight cluster during DAG construction (its triangles as GLOBAL
 * vertex ids, its accumulated object-space error, a centroid for spatial
 * grouping, and the index of its emitted record so a later level can patch
 * parent_error in). */
struct DagCluster {
    std::vector<uint32_t> tris;      /* 3N global vertex ids                */
    float                 own_error; /* object units, monotone up the DAG   */
    float                 c[3];      /* bounding-sphere centre (grouping)   */
    float                 gs[4];     /* creation-group sphere (own-test)    */
    size_t                slot;      /* index into out->desc/bounds/errors  */
};

/* Split `tris` (global-id triangle list) into <=124-tri meshlets and emit
 * each into `out` (grouped IB + desc + bounds + errors {own, ROOT}); the
 * created clusters are appended to `made` with their emit slots so parent
 * links can be patched later.  Returns the number of clusters emitted. */
static size_t dag_emit_clusters(const std::vector<uint32_t> &tris,
                                const float *positions, size_t vn,
                                float own_error,
                                const float *group_sphere, /* xyzw or NULL:
                                    leaves use each cluster's own sphere */
                                MeshMeshlets *out,
                                std::vector<DagCluster> *made)
{
    if (tris.size() < 3) return 0;
    const size_t max_meshlets = meshopt_buildMeshletsBound(
        tris.size(), JCE_MESHLET_MAX_VERTS, JCE_MESHLET_MAX_TRIS);
    std::vector<meshopt_Meshlet> mls(max_meshlets);
    std::vector<unsigned int>    mverts(max_meshlets * JCE_MESHLET_MAX_VERTS);
    std::vector<unsigned char>   mtris(max_meshlets * JCE_MESHLET_MAX_TRIS * 3);
    const size_t n = meshopt_buildMeshlets(
        mls.data(), mverts.data(), mtris.data(), tris.data(), tris.size(),
        positions, vn, 3 * sizeof(float),
        JCE_MESHLET_MAX_VERTS, JCE_MESHLET_MAX_TRIS, JCE_MESHLET_CONE_WEIGHT);
    for (size_t m = 0; m < n; ++m) {
        const meshopt_Meshlet &ml = mls[m];
        DagCluster dc;
        dc.own_error = own_error;
        dc.slot      = out->desc.size() / 2;
        dc.tris.reserve((size_t)ml.triangle_count * 3u);
        const uint32_t start = (uint32_t)out->indices.size();
        for (unsigned t = 0; t < ml.triangle_count * 3u; ++t) {
            uint32_t g = mverts[ml.vertex_offset + mtris[ml.triangle_offset + t]];
            out->indices.push_back(g);
            dc.tris.push_back(g);
        }
        out->desc.push_back(start);
        out->desc.push_back(ml.triangle_count * 3u);
        const meshopt_Bounds b = meshopt_computeMeshletBounds(
            &mverts[ml.vertex_offset], &mtris[ml.triangle_offset],
            ml.triangle_count, positions, vn, 3 * sizeof(float));
        out->bounds.push_back(b.center[0]);
        out->bounds.push_back(b.center[1]);
        out->bounds.push_back(b.center[2]);
        out->bounds.push_back(b.radius);
        out->bounds.push_back(b.cone_axis[0]);
        out->bounds.push_back(b.cone_axis[1]);
        out->bounds.push_back(b.cone_axis[2]);
        out->bounds.push_back(b.cone_cutoff);
        const float own_gs[4] = {
            group_sphere ? group_sphere[0] : b.center[0],
            group_sphere ? group_sphere[1] : b.center[1],
            group_sphere ? group_sphere[2] : b.center[2],
            group_sphere ? group_sphere[3] : b.radius,
        };
        out->errors.push_back(own_error);
        out->errors.push_back(JCE_MESHLET_ROOT_ERROR);  /* patched by parent */
        for (int a = 0; a < 4; ++a) out->errors.push_back(own_gs[a]);
        for (int a = 0; a < 4; ++a) out->errors.push_back(own_gs[a]);
        /* ^ parent-group sphere: placeholder (patched when a parent links;
         *   irrelevant for roots — parent_error stays +BIG). */
        dc.c[0] = b.center[0]; dc.c[1] = b.center[1]; dc.c[2] = b.center[2];
        for (int a = 0; a < 4; ++a) dc.gs[a] = own_gs[a];
        made->push_back(std::move(dc));
    }
    return n;
}

/* 30-bit Morton code of a cluster centroid quantised to the mesh AABB —
 * consecutive codes are spatial neighbours, so grouping consecutive runs of
 * sorted clusters yields compact merge groups without a graph partitioner. */
static uint32_t dag_morton(const float c[3], const float mn[3], const float inv[3])
{
    uint32_t out = 0;
    for (int a = 0; a < 3; ++a) {
        float f = (c[a] - mn[a]) * inv[a];
        uint32_t v = (uint32_t)(f < 0.0f ? 0.0f : (f > 1023.0f ? 1023.0f : f));
        for (int bit = 0; bit < 10; ++bit)
            out |= ((v >> bit) & 1u) << (bit * 3 + a);
    }
    return out;
}

static size_t generate_mesh_meshlets(const aiMesh *mesh, MeshMeshlets *out)
{
    if (!out) return 0;
    out->indices.clear(); out->desc.clear(); out->bounds.clear();
    out->errors.clear();
    if (!mesh || !mesh->HasPositions() || mesh->HasBones()) return 0;
    if (mesh->mNumFaces < JCE_MESHLET_COOK_MIN_TRIS) return 0;
    if (getenv("JCE_COOK_NO_MESHLETS") != NULL) return 0;

    const size_t vn = mesh->mNumVertices;
    std::vector<float> positions(vn * 3);
    for (size_t i = 0; i < vn; ++i) {
        positions[i * 3 + 0] = mesh->mVertices[i].x;
        positions[i * 3 + 1] = mesh->mVertices[i].y;
        positions[i * 3 + 2] = mesh->mVertices[i].z;
    }
    std::vector<uint32_t> base((size_t)mesh->mNumFaces * 3);
    for (size_t f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        if (face.mNumIndices != 3) return 0;    /* must be triangulated */
        base[f * 3 + 0] = face.mIndices[0];
        base[f * 3 + 1] = face.mIndices[1];
        base[f * 3 + 2] = face.mIndices[2];
    }

    /* Level 0: leaf clusters (own_error 0). */
    std::vector<DagCluster> cur;
    const size_t leaves = dag_emit_clusters(base, positions.data(), vn,
                                            0.0f, NULL, out, &cur);
    /* The runtime only dispatches at >= 16 clusters; don't pay the bytes
     * for a set it would never cull. */
    if (leaves < 16) {
        out->indices.clear(); out->desc.clear(); out->bounds.clear();
        out->errors.clear();
        return 0;
    }

    /* V3 cluster-LOD DAG: group -> boundary-locked simplify -> re-split. */
    size_t total = leaves, levels = 0;
    if (getenv("JCE_COOK_MESHLET_FLAT") == NULL) {
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        for (size_t i = 0; i < vn; ++i)
            for (int a = 0; a < 3; ++a) {
                float v = positions[i * 3 + a];
                if (v < mn[a]) mn[a] = v;
                if (v > mx[a]) mx[a] = v;
            }
        float inv[3];
        for (int a = 0; a < 3; ++a)
            inv[a] = 1023.0f / ((mx[a] - mn[a]) > 1e-9f ? (mx[a] - mn[a]) : 1.0f);

        std::vector<uint32_t> group_of(vn);    /* per-vertex owner group id  */
        std::vector<unsigned char> vlock(vn);
        for (levels = 1; levels <= JCE_MESHLET_DAG_MAX_LEVELS; ++levels) {
            if (cur.size() < 2) break;
            std::sort(cur.begin(), cur.end(),
                      [&](const DagCluster &a, const DagCluster &b) {
                          uint32_t ma = dag_morton(a.c, mn, inv);
                          uint32_t mb = dag_morton(b.c, mn, inv);
                          /* slot tie-break: equal quantised centroids would
                           * otherwise get STL-specific order, breaking
                           * cross-toolchain cooked-byte determinism. */
                          return ma != mb ? ma < mb : a.slot < b.slot;
                      });
            const size_t ngroups =
                (cur.size() + JCE_MESHLET_DAG_GROUP - 1) / JCE_MESHLET_DAG_GROUP;

            /* Boundary lock: a vertex referenced by two DIFFERENT groups of
             * this level is a group-boundary vertex — the simplifier must
             * not move it, so neighbouring groups (possibly selected at
             * different LOD levels at runtime) keep bit-identical seams. */
            const uint32_t NOG = 0xffffffffu;
            std::fill(group_of.begin(), group_of.end(), NOG);
            std::fill(vlock.begin(), vlock.end(), (unsigned char)0);
            for (size_t ci = 0; ci < cur.size(); ++ci) {
                const uint32_t g = (uint32_t)(ci / JCE_MESHLET_DAG_GROUP);
                for (uint32_t v : cur[ci].tris) {
                    if (group_of[v] == NOG)       group_of[v] = g;
                    else if (group_of[v] != g)    vlock[v] = 1;
                }
            }

            std::vector<DagCluster> next;
            std::vector<uint32_t> merged, simplified;
            for (size_t g = 0; g < ngroups; ++g) {
                const size_t b0 = g * JCE_MESHLET_DAG_GROUP;
                const size_t b1 = (b0 + JCE_MESHLET_DAG_GROUP < cur.size())
                                      ? b0 + JCE_MESHLET_DAG_GROUP : cur.size();
                if (b1 - b0 < 2) {
                    /* Singleton: carries to the next level ungrouped. */
                    next.push_back(std::move(cur[b0]));
                    continue;
                }
                merged.clear();
                float cmax = 0.0f;
                for (size_t ci = b0; ci < b1; ++ci) {
                    merged.insert(merged.end(), cur[ci].tris.begin(),
                                  cur[ci].tris.end());
                    if (cur[ci].own_error > cmax) cmax = cur[ci].own_error;
                }
                /* Group sphere = bound of the members' own-group spheres
                 * (nesting by construction: it encloses every descendant's
                 * test sphere, so coarser levels test at conservative,
                 * monotone distances). */
                float gsp[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                for (size_t ci = b0; ci < b1; ++ci)
                    for (int a = 0; a < 3; ++a)
                        gsp[a] += cur[ci].gs[a] / (float)(b1 - b0);
                for (size_t ci = b0; ci < b1; ++ci) {
                    float dx = cur[ci].gs[0] - gsp[0];
                    float dy = cur[ci].gs[1] - gsp[1];
                    float dz = cur[ci].gs[2] - gsp[2];
                    float dr = sqrtf(dx * dx + dy * dy + dz * dz) + cur[ci].gs[3];
                    if (dr > gsp[3]) gsp[3] = dr;
                }
                simplified.resize(merged.size());
                /* Sparse: the patch is ~500 tris out of possibly millions of
                 * mesh verts — without it every call pays O(vertex_count)
                 * remap/classify/rescale passes and the DAG build blows the
                 * bundle-cook budget on dense heroes.  Sparse changes the
                 * error normalisation to the SUBSET extent, so ErrorAbsolute
                 * keeps the errors lane in object units (no mscale). */
                float abs_err = 0.0f;
                size_t sn = meshopt_simplifyWithAttributes(
                    simplified.data(), merged.data(), merged.size(),
                    positions.data(), vn, 3 * sizeof(float),
                    NULL, 0, NULL, 0, vlock.data(),
                    (merged.size() / 6) * 3,      /* target: half the tris  */
                    1e30f,
                    meshopt_SimplifyLockBorder | meshopt_SimplifySparse |
                        meshopt_SimplifyErrorAbsolute,
                    &abs_err);
                if (sn >= (size_t)((double)merged.size() * 0.85) || sn < 3) {
                    /* Locked seams left nothing to collapse: the members
                     * stay DAG roots (parent_error remains +BIG). */
                    continue;
                }
                simplified.resize(sn);
                /* Parent error: own simplification error in object units
                 * (ErrorAbsolute — no rescale), forced monotone over the
                 * children (cut correctness). */
                const float perr = (abs_err > cmax ? abs_err : cmax) + 1e-7f;
                std::vector<DagCluster> made;
                if (dag_emit_clusters(simplified, positions.data(), vn,
                                      perr, gsp, out, &made) == 0)
                    continue;
                for (size_t ci = b0; ci < b1; ++ci) {
                    out->errors[cur[ci].slot * 10 + 1] = perr; /* link parent */
                    for (int a = 0; a < 4; ++a)
                        out->errors[cur[ci].slot * 10 + 6 + a] = gsp[a];
                }
                total += made.size();
                for (size_t mi2 = 0; mi2 < made.size(); ++mi2)
                    next.push_back(std::move(made[mi2]));
            }
            cur.swap(next);
        }
    }

    LOG_INFO(LOG_TAG,
             "meshlets '%s': %zu tris -> %zu leaf + %zu coarse clusters "
             "(%zu DAG level(s))",
             mesh->mName.C_Str(), base.size() / 3, leaves, total - leaves,
             levels ? levels - 1 : 0);
    return total;
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

static bool write_indexed_glb(const aiScene *scene, const MeshLods *mesh_lods,
                              const MeshMeshlets *mesh_meshlets,
                              uint8_t **out_buf, size_t *out_size)
{
    if (!scene || scene->mNumMeshes == 0) return false;
    /* JCE_COOK_FORCE_INDEXED_LOD: take the indexed + LOD-generating writer even for
     * textured/embedded-image scenes (textures are dropped — for GPU-vertex / LOD
     * benchmarking where geometry, not shading, is the subject). */
    if (getenv("JCE_COOK_FORCE_INDEXED_LOD") == NULL) {
        if (scene->mNumTextures > 0) return false;          /* embedded images: keep assimp path */
        for (unsigned i = 0; i < scene->mNumMaterials; ++i) {
            const aiMaterial *m = scene->mMaterials[i];
            if (m->GetTextureCount(aiTextureType_DIFFUSE) > 0 ||
                m->GetTextureCount(aiTextureType_BASE_COLOR) > 0)
                return false;                               /* textured: keep assimp path */
        }
    }

    std::vector<uint8_t> bin;
    std::string jbv, jacc, jmesh, jnode, jscene;
    int bv = 0, ac = 0, node = 0;
    bool any_lod = false;   /* set when any JCE_lod extension is emitted      */
    bool any_ml  = false;   /* set when any JCE_meshlets extension is emitted */

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

        /* indices (LOD0 / base) */
        jw_align4(bin); size_t ioff = bin.size();
        jw_put_bytes(bin, idx.data(), idx.size() * 4);
        jbv += ','; jw_appendf(jbv, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963}", ioff, idx.size() * 4);
        int idx_bv = bv++; jacc += ','; jw_appendf(jacc, "{\"bufferView\":%d,\"componentType\":5125,\"count\":%zu,\"type\":\"SCALAR\"}", idx_bv, idx.size());
        int idx_ac = ac++;

        /* LOD index accessors (additive — share this primitive's POSITION VB).
         * Each captured level becomes a SCALAR uint32 index accessor; the
         * accessor ids are listed in the primitive's JCE_lod extension so the
         * runtime can bind the reduced index set at distance.  Validity: every
         * LOD index references [0, V) (simplification only removes triangles /
         * reuses surviving vertices), so they are valid against the base VB. */
        std::string jlod;          /* "acc,acc,..." for this primitive */
        if (mesh_lods) {
            const MeshLods &ml = mesh_lods[mi];
            for (size_t l = 0; l < ml.levels.size(); ++l) {
                const std::vector<uint32_t> &li = ml.levels[l];
                if (li.size() < 3 || (li.size() % 3) != 0) continue;
                /* Defensive: all indices must address the base vertex range. */
                bool in_range = true;
                for (uint32_t v : li) { if (v >= V) { in_range = false; break; } }
                if (!in_range) continue;
                jw_align4(bin); size_t loff = bin.size();
                jw_put_bytes(bin, li.data(), li.size() * 4);
                jbv += ','; jw_appendf(jbv, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963}", loff, li.size() * 4);
                int l_bv = bv++; jacc += ','; jw_appendf(jacc, "{\"bufferView\":%d,\"componentType\":5125,\"count\":%zu,\"type\":\"SCALAR\"}", l_bv, li.size());
                int l_ac = ac++;
                if (!jlod.empty()) jlod += ',';
                jw_appendf(jlod, "%d", l_ac);
                any_lod = true;
            }
        }

        /* JCE_meshlets accessors (additive — grouped IB shares the base VB;
         * desc/bounds are raw little-endian SCALAR streams the loader reads
         * verbatim).  Validity mirrors the LOD lane: grouped indices must
         * address [0, V). */
        std::string jml;   /* "\"indices\":A,\"meshlets\":B,\"bounds\":C[,\"errors\":E]" */
        if (mesh_meshlets) {
            const MeshMeshlets &mm = mesh_meshlets[mi];
            bool ok = !mm.desc.empty() && !mm.indices.empty() &&
                      (mm.desc.size() % 2) == 0 &&
                      mm.bounds.size() == (mm.desc.size() / 2) * 8;
            const bool have_err =
                ok && mm.errors.size() == (mm.desc.size() / 2) * 10;
            for (size_t k = 0; ok && k < mm.indices.size(); ++k)
                if (mm.indices[k] >= V) ok = false;
            if (ok) {
                jw_align4(bin); size_t aoff = bin.size();
                jw_put_bytes(bin, mm.indices.data(), mm.indices.size() * 4);
                jbv += ','; jw_appendf(jbv, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963}", aoff, mm.indices.size() * 4);
                int a_bv = bv++; jacc += ','; jw_appendf(jacc, "{\"bufferView\":%d,\"componentType\":5125,\"count\":%zu,\"type\":\"SCALAR\"}", a_bv, mm.indices.size());
                int a_ac = ac++;
                jw_align4(bin); size_t doff = bin.size();
                jw_put_bytes(bin, mm.desc.data(), mm.desc.size() * 4);
                jbv += ','; jw_appendf(jbv, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", doff, mm.desc.size() * 4);
                int d_bv = bv++; jacc += ','; jw_appendf(jacc, "{\"bufferView\":%d,\"componentType\":5125,\"count\":%zu,\"type\":\"SCALAR\"}", d_bv, mm.desc.size());
                int d_ac = ac++;
                jw_align4(bin); size_t boff = bin.size();
                jw_put_bytes(bin, mm.bounds.data(), mm.bounds.size() * 4);
                jbv += ','; jw_appendf(jbv, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", boff, mm.bounds.size() * 4);
                int b_bv = bv++; jacc += ','; jw_appendf(jacc, "{\"bufferView\":%d,\"componentType\":5126,\"count\":%zu,\"type\":\"SCALAR\"}", b_bv, mm.bounds.size());
                int b_ac = ac++;
                jw_appendf(jml, "\"indices\":%d,\"meshlets\":%d,\"bounds\":%d", a_ac, d_ac, b_ac);
                if (have_err) {   /* V3 cluster-LOD DAG cut errors */
                    jw_align4(bin); size_t eoff = bin.size();
                    jw_put_bytes(bin, mm.errors.data(), mm.errors.size() * 4);
                    jbv += ','; jw_appendf(jbv, "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", eoff, mm.errors.size() * 4);
                    int e_bv = bv++; jacc += ','; jw_appendf(jacc, "{\"bufferView\":%d,\"componentType\":5126,\"count\":%zu,\"type\":\"SCALAR\"}", e_bv, mm.errors.size());
                    int e_ac = ac++;
                    jw_appendf(jml, ",\"errors\":%d", e_ac);
                }
                any_ml = true;
            }
        }

        if (!jmesh.empty()) jmesh += ',';
        jw_appendf(jmesh, "{\"primitives\":[{\"attributes\":{\"POSITION\":%d", pos_ac);
        if (nrm_ac >= 0) jw_appendf(jmesh, ",\"NORMAL\":%d", nrm_ac);
        if (uv_ac  >= 0) jw_appendf(jmesh, ",\"TEXCOORD_0\":%d", uv_ac);
        jw_appendf(jmesh, "},\"indices\":%d,\"material\":%u,\"mode\":4", idx_ac, me->mMaterialIndex);
        if (!jlod.empty() || !jml.empty()) {
            jmesh += ",\"extensions\":{";
            if (!jlod.empty())
                jw_appendf(jmesh, "\"JCE_lod\":{\"indices\":[%s]}", jlod.c_str());
            if (!jml.empty()) {
                if (!jlod.empty()) jmesh += ',';
                jw_appendf(jmesh, "\"JCE_meshlets\":{%s}", jml.c_str());
            }
            jmesh += "}";
        }
        jmesh += "}]}";

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
        /* Preserve two-sidedness: stylized content (tent canvas, foliage
         * cards) authors doubleSided=true; dropping it back-face-culls open
         * single-layer sheets into holes. */
        int two_sided = 0;
        {
            unsigned mx = 1;
            aiGetMaterialIntegerArray(scene->mMaterials[i], AI_MATKEY_TWOSIDED,
                                      &two_sided, &mx);
        }
        if (!jmat.empty()) jmat += ',';
        jw_appendf(jmat, "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[%.6g,%.6g,%.6g,%.6g],"
                   "\"metallicFactor\":0.0,\"roughnessFactor\":1.0}%s}",
                   kd.r, kd.g, kd.b, kd.a,
                   two_sided ? ",\"doubleSided\":true" : "");
    }

    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"jce-indexed-glb\"},";
    /* Declare the vendor extensions so spec-compliant readers (and our loader)
     * know they are OPTIONAL — they appear in extensionsUsed but NOT
     * extensionsRequired, so a reader that ignores them still loads LOD0. */
    if (any_lod || any_ml) {
        json += "\"extensionsUsed\":[";
        if (any_lod) json += "\"JCE_lod\"";
        if (any_ml)  { if (any_lod) json += ','; json += "\"JCE_meshlets\""; }
        json += "],";
    }
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

    /* Bake per-node TRS into the vertices for STATIC scenes.  The indexed-glb
     * writer below walks only scene->mMeshes and emits identity nodes
     * ({"mesh":N}, no matrix/TRS), so any transform left on aiNodes — e.g.
     * the Z-up→Y-up rotation node (quat -0.7071,0,0,0.7071) Draco/DCC-
     * exported glTFs carry — would be silently dropped and the converted
     * model would render displaced/lying flat.  NOTE: this flag cannot be
     * OR'd into `flags` above: assimp rejects PreTransformVertices together
     * with OptimizeGraph (whole import returns NULL); a second
     * ApplyPostProcessing pass validates only its own flag.  Skinned or
     * animated scenes are skipped (PreTransformVertices deletes bones and
     * animations); those keep their hierarchy for the glb2 fallback path. */
    {
        bool animated = scene->mNumAnimations > 0;
        for (unsigned i = 0; !animated && i < scene->mNumMeshes; ++i)
            animated = scene->mMeshes[i]->HasBones();
        if (!animated) {
            scene = importer.ApplyPostProcessing(aiProcess_PreTransformVertices);
            if (!scene) {
                LOG_WARN(LOG_TAG, "assimp PreTransformVertices failed: %s",
                         importer.GetErrorString());
                return 0;
            }
        }
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
     * each finalised mesh and CAPTURE the chain per-mesh so the indexed-glb
     * writer can persist it.  Generation does not mutate the scene, so the
     * exported base GLB geometry is byte-identical to before. */
    std::vector<MeshLods> mesh_lods(scene->mNumMeshes);
    {
        size_t total_lods = 0;
        for (unsigned i = 0; i < scene->mNumMeshes; ++i)
            total_lods += generate_mesh_lods(scene->mMeshes[i], &mesh_lods[i]);
        if (total_lods > 0)
            LOG_INFO(LOG_TAG, "auto-LOD: generated %zu LOD level(s) across %u mesh(es)",
                     total_lods, scene->mNumMeshes);
    }

    /* AUTO-MESHLETS (additive, Nanite-lite V1): cluster-cull sidecars for
     * dense static meshes, persisted as the JCE_meshlets primitive extension
     * by the indexed writer below.  Never mutates the scene. */
    std::vector<MeshMeshlets> mesh_meshlets(scene->mNumMeshes);
    {
        size_t total_ml = 0;
        for (unsigned i = 0; i < scene->mNumMeshes; ++i)
            total_ml += generate_mesh_meshlets(scene->mMeshes[i], &mesh_meshlets[i]);
        if (total_ml > 0)
            LOG_INFO(LOG_TAG, "auto-meshlets: %zu cluster(s) across %u mesh(es)",
                     total_ml, scene->mNumMeshes);
    }

    /* Preferred path: write an INDEXED glb ourselves (Assimp's glTF2 exporter
     * un-indexes to triangle soup, inflating the bundle 2-6x).  Falls back to
     * the Assimp exporter for textured / embedded-image scenes.  The captured
     * LOD chains are persisted into the glb (JCE_lod primitive extension). */
    if (write_indexed_glb(scene, mesh_lods.data(), mesh_meshlets.data(),
                          out_buf, out_size))
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
