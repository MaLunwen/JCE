/*
 * jce_model_importer.cpp  Model loading via assimp (engine layer).
 *
 * Uses assimp's C++ interface to load models from disk or PAK assets.
 * Supports all assimp-supported formats: OBJ, FBX, 3DS, glTF, etc.
 */

#include <jce/resource/jce_model_importer.h>

#include <exception>   /* std::exception — extern "C" firewall */
#include <stdexcept>
#include <utility>     /* std::forward                        */

#include <jce/os/core/jce_filesystem.h>
/* The skinned-import path (guarded by JCE_MODEL_IMPORTER_COOK_ONLY) is the
 * sole consumer of jce_math / jce_skinned_mesh / jce_skeleton types; the static
 * model-import + collider-cook path the host jce_cook tool uses needs none of
 * them, so guard their headers out of the cook-only build. */
#ifndef JCE_MODEL_IMPORTER_COOK_ONLY
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_skinned_mesh.h>
#include <jce/middleware/animation/jce_skeleton.h>
#endif

extern "C" {
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_json.h>  /* the extracted-texture colour-space sidecar */
#include "jce_model_import_settings.h"
#include <jce/os/core/jce_log.h>
#ifndef JCE_MODEL_IMPORTER_COOK_ONLY
/* Renderer-internal CPU-model builder + GPU upload.  jce_resource PUBLICLY
 * links jce_renderer / jce_animation, so reaching these is a sanctioned
 * bridge-layer edge (this layer is the asset pipeline that feeds the renderer);
 * the layer-dependency lint scopes its renderer/middleware/os rules and does
 * not police engine/src/resource/.  The skinned FBX path reuses the SAME
 * JceModelCpu intermediate + jce_gltf_upload_cpu the glTF loader already owns,
 * so no skinned-consumption code is duplicated.  Guarded out of the host
 * jce_cook build (JCE_MODEL_IMPORTER_COOK_ONLY): the cook tool links neither
 * jce_renderer nor jce_animation, so it must not reference these symbols. */
#include "renderer/jce_gltf_loader.h"
#include "middleware/animation/jce_animation.h"
#include <jce/middleware/animation/jce_anim_compress.h>  /* opt-in keyframe reduction */
#endif
}

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

#define LOG_TAG "model_importer"

/*
 * Convert an absolute (or any) path to a CWD-relative path.
 * Falls back to the original string if fs::relative fails.
 */
static std::string to_cwd_relative(const std::string &p)
{
    try {
        fs::path abs(p);
        if (!abs.is_absolute()) return p;
        fs::path rel = fs::relative(abs, fs::current_path());
        if (!rel.empty() && rel != ".")
            return rel.string();
    } catch (...) {}
    return p;
}

/* ------------------------------------------------------------------ *
 *  Exception firewall for the extern "C" surface.
 *
 *  Every function below is reached across the flat C ABI, and an exception
 *  crossing that boundary is undefined behaviour — in practice std::terminate
 *  with a stack the C caller cannot interpret.  This TU is the one that feeds
 *  UNTRUSTED bytes to a C++ library: model files arrive from a PAK, from disk,
 *  or from whatever a game ships, and the body allocates std::vector /
 *  std::string per mesh while doing it.  std::bad_alloc on a malformed or
 *  hostile file is not exotic here, it is the expected failure.
 *
 *  Same shape as the firewalls added to jce_bt_impl.cpp (BT registerBuilder /
 *  tickOnce / haltTree): log what happened, then return the value the C
 *  caller already knows how to handle as failure.
 * ------------------------------------------------------------------ */
template <typename R, typename Fn>
static R mi_guard(const char *what, R on_error, Fn &&fn)
{
    try {
        return fn();
    } catch (const std::exception &e) {
        LOG_ERROR(LOG_TAG, "%s: C++ exception escaped (%s) — reporting failure",
                  what, e.what());
    } catch (...) {
        LOG_ERROR(LOG_TAG, "%s: non-std exception escaped — reporting failure",
                  what);
    }
    return on_error;
}

/* void-returning entry points: nothing to report, but an escaping exception
 * would still terminate the process. */
template <typename Fn>
static void mi_guard_void(const char *what, Fn &&fn)
{
    try {
        fn();
    } catch (const std::exception &e) {
        LOG_ERROR(LOG_TAG, "%s: C++ exception escaped (%s)", what, e.what());
    } catch (...) {
        LOG_ERROR(LOG_TAG, "%s: non-std exception escaped", what);
    }
}

extern "C" {

void jce_model_importer_free_cpu(JceModelCpuMeshData *data)
{
    if (!data) return;
    JCE_FREE(data->vertices);
    JCE_FREE(data->indices);
    data->vertices = nullptr;
    data->indices = nullptr;
    data->vertex_count = 0;
    data->index_count = 0;
}

static bool build_cpu_mesh_data(const aiScene *scene, JceModelCpuMeshData *out)
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
        JCE_CALLOC(total_verts, sizeof(JceMeshVertex)));
    auto *indices = (total_indices > 0)
        ? static_cast<uint32_t *>(JCE_MALLOC(
            static_cast<size_t>(total_indices) * sizeof(uint32_t)))
        : nullptr;
    if (!verts || (total_indices > 0 && !indices)) {
        JCE_FREE(verts);
        JCE_FREE(indices);
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
    JceModelCpuMeshData cpu = {};
    if (!build_cpu_mesh_data(scene, &cpu))
        return nullptr;

    JceMesh *mesh = jce_mesh_create(cpu.vertices, cpu.vertex_count,
                                    cpu.indices, cpu.index_count);
    jce_model_importer_free_cpu(&cpu);
    return mesh;
}

/* ─── Per-part extraction ─────────────────────────────────────────
 * Walk the node tree (no PreTransformVertices) and emit one part per
 * node that owns geometry, merging that node's meshes and recording the
 * accumulated world transform. */

/* assimp aiMatrix4x4 is row-major; jce wants column-major float[16]. */
static void ai_to_colmajor(const aiMatrix4x4 &m, float out[16])
{
    out[0]  = m.a1; out[1]  = m.b1; out[2]  = m.c1; out[3]  = m.d1;
    out[4]  = m.a2; out[5]  = m.b2; out[6]  = m.c2; out[7]  = m.d2;
    out[8]  = m.a3; out[9]  = m.b3; out[10] = m.c3; out[11] = m.d3;
    out[12] = m.a4; out[13] = m.b4; out[14] = m.c4; out[15] = m.d4;
}

/* Count how many nodes in the subtree carry at least one mesh. */
static uint32_t count_geo_nodes(const aiNode *node)
{
    if (!node) return 0;
    uint32_t n = (node->mNumMeshes > 0) ? 1u : 0u;
    for (unsigned i = 0; i < node->mNumChildren; i++)
        n += count_geo_nodes(node->mChildren[i]);
    return n;
}

/* Emit one part for `node` (merging its meshes), then recurse. `parent`
 * is the accumulated world transform of the parent chain. Returns false
 * only on allocation failure. */
static bool emit_node_parts(const aiScene *scene, const aiNode *node,
                            const aiMatrix4x4 &parent,
                            JceModelParts *out, uint32_t *cursor)
{
    aiMatrix4x4 world = parent * node->mTransformation;

    if (node->mNumMeshes > 0) {
        uint32_t total_verts = 0, total_tris = 0;
        for (unsigned i = 0; i < node->mNumMeshes; i++) {
            const aiMesh *ai = scene->mMeshes[node->mMeshes[i]];
            total_verts += ai->mNumVertices;
            for (unsigned f = 0; f < ai->mNumFaces; f++)
                if (ai->mFaces[f].mNumIndices == 3) total_tris += 1;
        }

        if (total_verts > 0) {
            JceModelPart *part = &out->parts[*cursor];

            const char *nm = (node->mName.length > 0)
                ? node->mName.data : "part";
            snprintf(part->name, sizeof(part->name), "%s", nm);
            ai_to_colmajor(world, part->transform);

            float *pos = static_cast<float *>(
                JCE_MALLOC(static_cast<size_t>(total_verts) * 3 * sizeof(float)));
            uint32_t *idx = (total_tris > 0)
                ? static_cast<uint32_t *>(JCE_MALLOC(
                    static_cast<size_t>(total_tris) * 3 * sizeof(uint32_t)))
                : nullptr;
            if (!pos || (total_tris > 0 && !idx)) {
                JCE_FREE(pos);
                JCE_FREE(idx);
                return false;
            }

            uint32_t voff = 0, icnt = 0;
            for (unsigned i = 0; i < node->mNumMeshes; i++) {
                const aiMesh *ai = scene->mMeshes[node->mMeshes[i]];
                for (unsigned v = 0; v < ai->mNumVertices; v++) {
                    pos[(voff + v) * 3 + 0] = ai->mVertices[v].x;
                    pos[(voff + v) * 3 + 1] = ai->mVertices[v].y;
                    pos[(voff + v) * 3 + 2] = ai->mVertices[v].z;
                }
                for (unsigned f = 0; f < ai->mNumFaces; f++) {
                    const aiFace &face = ai->mFaces[f];
                    if (face.mNumIndices != 3) continue;
                    idx[icnt++] = face.mIndices[0] + voff;
                    idx[icnt++] = face.mIndices[1] + voff;
                    idx[icnt++] = face.mIndices[2] + voff;
                }
                voff += ai->mNumVertices;
            }

            part->positions    = pos;
            part->vertex_count = total_verts;
            part->indices      = idx;
            part->index_count  = icnt;
            (*cursor)++;
        }
    }

    for (unsigned i = 0; i < node->mNumChildren; i++)
        if (!emit_node_parts(scene, node->mChildren[i], world, out, cursor))
            return false;
    return true;
}

static bool build_parts(const aiScene *scene, JceModelParts *out)
{
    out->parts = nullptr;
    out->count = 0;
    if (!scene || !scene->mRootNode) return false;

    uint32_t node_count = count_geo_nodes(scene->mRootNode);
    if (node_count == 0) return false;

    out->parts = static_cast<JceModelPart *>(
        JCE_CALLOC(node_count, sizeof(JceModelPart)));
    if (!out->parts) return false;

    aiMatrix4x4 identity;   /* default ctor is identity */
    uint32_t cursor = 0;
    if (!emit_node_parts(scene, scene->mRootNode, identity, out, &cursor)) {
        jce_model_importer_free_parts(out);
        return false;
    }
    out->count = cursor;
    if (cursor == 0) {
        jce_model_importer_free_parts(out);
        return false;
    }
    return true;
}

/* FBX is authored in centimeters.  Enabling aiProcess_GlobalScale makes Assimp
 * apply the file's UnitScaleFactor so geometry arrives in meters, matching
 * glTF/OBJ (which are already metric / unit-agnostic).  Verified empirically:
 * the BagMan character FBX measures 209.94 raw -> 2.0994 with this flag, exactly
 * the same character's GLB.  Do NOT also set AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY:
 * the FBX UnitScaleFactor is already 0.01, so an extra 0.01 shrinks 100x too
 * much (209.94 -> 0.021).  `hint` may be a path, ".fbx" or "fbx" — only the
 * trailing extension chars are tested. */
static unsigned fbx_scale_flags(const char *hint)
{
    if (!hint) return 0;
    size_t n = strlen(hint);
    if (n < 3) return 0;
    const char *t = hint + n - 3;
    if ((t[0] == 'f' || t[0] == 'F') &&
        (t[1] == 'b' || t[1] == 'B') &&
        (t[2] == 'x' || t[2] == 'X'))
        return aiProcess_GlobalScale;
    return 0;
}

/* Postprocess flags shared by the part loaders: deliberately WITHOUT
 * aiProcess_PreTransformVertices so node separation survives. */
static unsigned parts_postprocess_flags(void)
{
    return aiProcess_Triangulate
         | aiProcess_GenSmoothNormals
         | aiProcess_FlipUVs;
}

static bool jce_model_importer_load_parts_memory_impl(const void *data, size_t size,
                                          const char *ext_hint,
                                          JceModelParts *out);

bool jce_model_importer_load_parts_memory(const void *data, size_t size,
                                          const char *ext_hint,
                                          JceModelParts *out)
{
    return mi_guard("load_parts_memory", false, [&] {
        return jce_model_importer_load_parts_memory_impl(data, size, ext_hint, out);
    });
}

static bool jce_model_importer_load_parts_memory_impl(const void *data, size_t size,
                                          const char *ext_hint,
                                          JceModelParts *out)
{
    if (!data || size == 0 || !out) return false;
    out->parts = nullptr;
    out->count = 0;

    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFileFromMemory(
        data, size, parts_postprocess_flags() | fbx_scale_flags(ext_hint),
        ext_hint ? ext_hint : "");
    if (!scene || !scene->mNumMeshes) {
        LOG_ERROR(LOG_TAG, "assimp parts load failed: %s",
                  importer.GetErrorString());
        return false;
    }
    return build_parts(scene, out);
}

static bool jce_model_importer_load_parts_file_impl(const char *file_path,
                                        JceModelParts *out);

bool jce_model_importer_load_parts_file(const char *file_path,
                                        JceModelParts *out)
{
    return mi_guard("load_parts_file", false, [&] {
        return jce_model_importer_load_parts_file_impl(file_path, out);
    });
}

static bool jce_model_importer_load_parts_file_impl(const char *file_path,
                                        JceModelParts *out)
{
    if (!file_path || file_path[0] == '\0' || !out) return false;
    out->parts = nullptr;
    out->count = 0;

    Assimp::Importer importer;
    const aiScene *scene = nullptr;

    /* Try host-FS read first (mirrors load_cpu_file), else direct read. */
    void *vbuf = nullptr;
    uint64_t vsz = 0;
    vbuf = jce_fs_host_read_all(file_path, &vsz);
    if (vbuf && vsz > 0) {
        const char *ext = "";
        const char *dot = strrchr(file_path, '.');
        if (dot) ext = dot;
        scene = importer.ReadFileFromMemory(
            vbuf, (size_t)vsz,
            parts_postprocess_flags() | fbx_scale_flags(ext), ext);
    }
    if (!scene)
        scene = importer.ReadFile(
            file_path, parts_postprocess_flags() | fbx_scale_flags(file_path));

    bool ok = false;
    if (!scene || !scene->mNumMeshes) {
        LOG_ERROR(LOG_TAG, "assimp parts file load failed: %s  %s",
                  file_path, importer.GetErrorString());
    } else {
        ok = build_parts(scene, out);
    }
    if (vbuf) jce_fs_buffer_free(vbuf);
    return ok;
}

void jce_model_importer_free_parts(JceModelParts *parts)
{
    if (!parts || !parts->parts) {
        if (parts) { parts->parts = nullptr; parts->count = 0; }
        return;
    }
    for (uint32_t i = 0; i < parts->count; i++) {
        JCE_FREE(parts->parts[i].positions);
        JCE_FREE(parts->parts[i].indices);
    }
    JCE_FREE(parts->parts);
    parts->parts = nullptr;
    parts->count = 0;
}

static bool jce_model_importer_load_cpu_file_impl(const char *file_path,
                                    JceModelCpuMeshData *out);

bool jce_model_importer_load_cpu_file(const char *file_path,
                                    JceModelCpuMeshData *out)
{
    return mi_guard("load_cpu_file", false, [&] {
        return jce_model_importer_load_cpu_file_impl(file_path, out);
    });
}

static bool jce_model_importer_load_cpu_file_impl(const char *file_path,
                                    JceModelCpuMeshData *out)
{
    if (!file_path || file_path[0] == '\0' || !out) return false;

    out->vertices = nullptr;
    out->indices = nullptr;
    out->vertex_count = 0;
    out->index_count = 0;

    /* THE ASSET'S OWN IMPORT OPTIONS.  These were a constant flag word; the
     * editor has been writing <asset>.import.json for every model an import
     * preset touched, and nothing read it -- so "do not flip UVs" was written
     * to disk exactly as asked and every model still imported flipped.
     * Defaults equal the old constants, so a model with no sidecar is
     * unchanged. */
    JceModelImportSettings imp;
    (void)jce_model_import_settings_load(file_path, &imp);

    Assimp::Importer importer;
    const aiScene *scene = nullptr;

    /* VFS-first: if an active filesystem override is installed (editor
     * scene preview from a .jbundle), satisfy the read through it and
     * hand Assimp an in-memory buffer.  Falls back to direct ReadFile
     * on miss so on-disk projects keep working unchanged. */
    void   *vbuf = nullptr;
    {
        uint64_t vsz = 0;
        vbuf = jce_fs_host_read_all(file_path, &vsz);
        if (vbuf && vsz > 0) {
            const char *ext = "";
            const char *dot = strrchr(file_path, '.');
            if (dot) ext = dot;
            scene = importer.ReadFileFromMemory(
                vbuf, (size_t)vsz,
                jce_model_import_assimp_flags(&imp, ext),
                ext);
        }
    }

    if (!scene) {
        scene = importer.ReadFile(
            file_path,
            jce_model_import_assimp_flags(&imp, file_path));
    }

    if (!scene || !scene->mNumMeshes) {
        LOG_ERROR(LOG_TAG, "assimp cpu file load failed: %s  %s",
                  file_path, importer.GetErrorString());
        if (vbuf) jce_fs_buffer_free(vbuf);
        return false;
    }

    if (!build_cpu_mesh_data(scene, out)) {
        LOG_ERROR(LOG_TAG, "cpu mesh conversion failed: %s", file_path);
        if (vbuf) jce_fs_buffer_free(vbuf);
        return false;
    }

    /* The authored uniform scale, applied AFTER assimp.
     *
     * Not folded into AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY: that key multiplies
     * with an FBX's own UnitScaleFactor, and this file already carries a
     * comment about what that combination cost (209.94 -> 0.021, a 100x
     * over-shrink).  Positions only -- normals and tangents are directions and
     * a uniform scale leaves them unchanged. */
    if (imp.scale != 1.0f && out->vertices) {
        for (uint32_t i = 0; i < out->vertex_count; ++i) {
            out->vertices[i].pos[0] *= imp.scale;
            out->vertices[i].pos[1] *= imp.scale;
            out->vertices[i].pos[2] *= imp.scale;
        }
    }

    LOG_DEBUG(LOG_TAG, "decoded cpu mesh %s (%u verts, %u tris)",
              file_path, out->vertex_count, out->index_count / 3);
    if (vbuf) jce_fs_buffer_free(vbuf);
    return true;
}

JceMesh *jce_model_importer_load_pak(const JcePakArchive *pak, const char *asset_path)
{
    if (!pak || !asset_path) return nullptr;

    const JcePakAsset *asset = jce_pak_find(pak, asset_path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "model not found in PAK: %s", asset_path);
        return nullptr;
    }

    void *buf = JCE_MALLOC(static_cast<size_t>(asset->original_size));
    if (!buf) return nullptr;

    size_t n = jce_pak_decompress(asset, buf, static_cast<size_t>(asset->original_size));
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", asset_path);
        JCE_FREE(buf);
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
        | aiProcess_CalcTangentSpace
        | aiProcess_PreTransformVertices
        | fbx_scale_flags(ext),
        ext);

    JCE_FREE(buf);

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

JceMesh *jce_model_importer_load_file(const char *file_path)
{
    if (!file_path || file_path[0] == '\0') return nullptr;

    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(
        file_path,
        aiProcess_Triangulate
        | aiProcess_GenSmoothNormals
        | aiProcess_FlipUVs
        | aiProcess_CalcTangentSpace
        | aiProcess_PreTransformVertices
        | fbx_scale_flags(file_path));

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

#ifndef JCE_MODEL_IMPORTER_COOK_ONLY
/* ══════════════════════════════════════════════════════════════════════
 *  SKINNED / ANIMATED MODEL IMPORT (FBX, DAE, …)
 *  (compiled out of the host jce_cook build via JCE_MODEL_IMPORTER_COOK_ONLY:
 *   this whole region references jce_renderer / jce_animation symbols that the
 *   lean cook tool does not link; the static import + collider-cook path above
 *   stays outside the guard and references neither.)
 * ----------------------------------------------------------------------
 *  Mirrors the cgltf skeletal extraction in
 *  engine/src/renderer/jce_gltf_loader.c (extract_skeleton /
 *  build_primitive_cpu / extract_animations), producing the SAME JceModelCpu
 *  intermediate via the renderer's CPU-model builder so the existing
 *  jce_gltf_upload_cpu GPU path and the renderer/animation consumers work
 *  unchanged.  Everything in here is bgfx-free (the GPU upload is a separate,
 *  render-thread step), so the extraction is exercisable headless.
 * ══════════════════════════════════════════════════════════════════════ */

/* Does any mesh in the scene carry skinning bones? */
static bool scene_is_skinned(const aiScene *scene)
{
    if (!scene) return false;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m)
        if (scene->mMeshes[m] && scene->mMeshes[m]->mNumBones > 0)
            return true;
    return false;
}

/* Postprocess flags for the SKINNED path: like the static loaders' flags but
 * WITHOUT aiProcess_PreTransformVertices (which would flatten the hierarchy and
 * discard bones).  LimitBoneWeights caps influences to 4 (== JceSkinnedVertex);
 * PopulateArmatureData fills aiBone.mArmature/mNode for armature discovery. */
static unsigned skinned_postprocess_flags(const char *hint)
{
    return aiProcess_Triangulate
         | aiProcess_GenSmoothNormals
         | aiProcess_CalcTangentSpace
         | aiProcess_FlipUVs
         | aiProcess_LimitBoneWeights
         | aiProcess_PopulateArmatureData
         | fbx_scale_flags(hint);
}

/* Minimal postprocess flags for the SKINNED path RETRY: the geometry-heavy
 * passes (Triangulate / GenSmoothNormals / CalcTangentSpace / FlipUVs) can make
 * assimp return NULL on an animation-only FBX (no renderable mesh — the standard
 * separate per-clip animation workflow: a base mesh+skeleton FBX plus Idle / Run
 * / Walk / Attack files that contain only node keyframes).  Drop them on the
 * retry and keep ONLY the cm->m GlobalScale + LimitBoneWeights (harmless without
 * a mesh) + PopulateArmatureData (helps bone/node resolution for anim-only). */
static unsigned skinned_minimal_flags(const char *hint)
{
    return aiProcess_LimitBoneWeights
         | aiProcess_PopulateArmatureData
         | fbx_scale_flags(hint);
}

/* Find a node by name in the aiNode hierarchy (case-sensitive, exact). */
static const aiNode *find_node_by_name(const aiNode *node, const char *name)
{
    if (!node) return nullptr;
    if (node->mName.length > 0 && strcmp(node->mName.C_Str(), name) == 0)
        return node;
    for (unsigned i = 0; i < node->mNumChildren; ++i) {
        const aiNode *r = find_node_by_name(node->mChildren[i], name);
        if (r) return r;
    }
    return nullptr;
}

/* Skeleton + skin extraction state shared across the per-mesh passes. */
struct SkinBuild {
    std::vector<const aiNode *>            joint_nodes;  /* index = joint index */
    std::unordered_map<std::string, int>  name_to_joint;
    std::unordered_map<const aiNode *, int> node_to_joint;
};

/* Mark `node` and all its ancestors as required joints (set membership only). */
static void collect_joint_with_ancestors(const aiNode *node,
                                         std::unordered_map<const aiNode *, bool> &want)
{
    while (node) {
        want[node] = true;
        node = node->mParent;
    }
}

/* Emit the joint list in parent-before-child order: pre-order DFS over the
 * aiNode tree, keeping only nodes flagged in `want`.  Records the index maps. */
static void emit_joints_preorder(const aiNode *node,
                                 const std::unordered_map<const aiNode *, bool> &want,
                                 SkinBuild *sb)
{
    if (!node) return;
    auto it = want.find(node);
    if (it != want.end() && it->second) {
        int idx = (int)sb->joint_nodes.size();
        sb->joint_nodes.push_back(node);
        sb->node_to_joint[node] = idx;
        if (node->mName.length > 0)
            sb->name_to_joint[node->mName.C_Str()] = idx;
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        emit_joints_preorder(node->mChildren[i], want, sb);
}

/* Build the joint set (union of all bone nodes + their ancestor chains) and the
 * ordered joint list.  Returns false if no bones resolve to nodes. */
static bool build_skin_joint_set(const aiScene *scene, SkinBuild *sb)
{
    std::unordered_map<const aiNode *, bool> want;

    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh *mesh = scene->mMeshes[m];
        if (!mesh) continue;
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const aiBone *bone = mesh->mBones[b];
            if (!bone) continue;
            /* PopulateArmatureData fills mNode; fall back to a name lookup. */
            const aiNode *bn = bone->mNode;
            if (!bn && scene->mRootNode)
                bn = find_node_by_name(scene->mRootNode, bone->mName.C_Str());
            if (bn) collect_joint_with_ancestors(bn, want);
        }
    }

    if (want.empty() || !scene->mRootNode) return false;

    /* Do NOT include the scene root itself unless it is a real bone ancestor
     * that carries a non-identity transform we need; the pre-order walk starts
     * at the root and only emits flagged nodes, so the root is emitted only when
     * it was flagged (i.e. it lies on a bone's ancestor chain). */
    emit_joints_preorder(scene->mRootNode, want, sb);

    /* Cap to the GPU bone-palette ceiling (jce_skeleton_create also rejects
     * over-ceiling skeletons; clamp here so vertex joint indices stay in range
     * for the uint8 JceSkinnedVertex.joints[]). */
    if (sb->joint_nodes.size() > (size_t)JCE_MAX_BONES) {
        LOG_WARN(LOG_TAG,
                 "skinned import: %u joints exceeds JCE_MAX_BONES (%d); clamping",
                 (unsigned)sb->joint_nodes.size(), JCE_MAX_BONES);
        sb->joint_nodes.resize((size_t)JCE_MAX_BONES);
        /* Rebuild the name/node maps for the surviving joints only. */
        sb->name_to_joint.clear();
        sb->node_to_joint.clear();
        for (size_t i = 0; i < sb->joint_nodes.size(); ++i) {
            const aiNode *n = sb->joint_nodes[i];
            sb->node_to_joint[n] = (int)i;
            if (n->mName.length > 0) sb->name_to_joint[n->mName.C_Str()] = (int)i;
        }
    }

    return !sb->joint_nodes.empty();
}

/* Anim-only joint set: when a scene carries animation channels but NO skinned
 * mesh (the standard "separate per-clip animation FBX" workflow — a base
 * mesh+skeleton file plus Idle/Run/Walk/Attack files that contain only node
 * keyframes), build the joint set from the union of every animated node + its
 * ancestor chain, using the SAME pre-order emit + JCE_MAX_BONES clamp as the
 * mesh path.  Joints get an identity inverse-bind in build_skeleton (no bone
 * offset matrices exist), which is correct for an animation-source / retarget
 * model.  Returns false if no animation channel resolves to a node. */
static bool build_anim_joint_set(const aiScene *scene, SkinBuild *sb)
{
    if (!scene || !scene->mRootNode || scene->mNumAnimations == 0) return false;

    std::unordered_map<const aiNode *, bool> want;

    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation *anim = scene->mAnimations[a];
        if (!anim) continue;
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim *na = anim->mChannels[c];
            if (!na) continue;
            const aiNode *tn =
                find_node_by_name(scene->mRootNode, na->mNodeName.C_Str());
            if (tn) collect_joint_with_ancestors(tn, want);
        }
    }

    if (want.empty()) return false;

    emit_joints_preorder(scene->mRootNode, want, sb);

    /* Same GPU bone-palette clamp as the mesh path. */
    if (sb->joint_nodes.size() > (size_t)JCE_MAX_BONES) {
        LOG_WARN(LOG_TAG,
                 "anim-only import: %u joints exceeds JCE_MAX_BONES (%d); clamping",
                 (unsigned)sb->joint_nodes.size(), JCE_MAX_BONES);
        sb->joint_nodes.resize((size_t)JCE_MAX_BONES);
        sb->name_to_joint.clear();
        sb->node_to_joint.clear();
        for (size_t i = 0; i < sb->joint_nodes.size(); ++i) {
            const aiNode *n = sb->joint_nodes[i];
            sb->node_to_joint[n] = (int)i;
            if (n->mName.length > 0) sb->name_to_joint[n->mName.C_Str()] = (int)i;
        }
    }

    return !sb->joint_nodes.empty();
}

/* Build a JceSkeleton from the ordered joint set.  inverse_bind comes from the
 * aiBone.mOffsetMatrix (the only place it lives); joints not referenced by any
 * bone (pure ancestor nodes) get an identity inverse-bind.  rest TRS is the
 * node's local transform. */
static JceSkeleton *build_skeleton(const aiScene *scene, const SkinBuild *sb)
{
    uint32_t num_joints = (uint32_t)sb->joint_nodes.size();
    if (num_joints == 0) return nullptr;

    JceJoint *joints = (JceJoint *)JCE_CALLOC(num_joints, sizeof(JceJoint));
    if (!joints) return nullptr;

    /* Gather each bone's offset matrix keyed by node, so ancestor-only joints
     * (no bone) correctly default to identity. */
    std::unordered_map<const aiNode *, aiMatrix4x4> offset_by_node;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh *mesh = scene->mMeshes[m];
        if (!mesh) continue;
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const aiBone *bone = mesh->mBones[b];
            if (!bone) continue;
            const aiNode *bn = bone->mNode;
            if (!bn && scene->mRootNode)
                bn = find_node_by_name(scene->mRootNode, bone->mName.C_Str());
            if (bn && offset_by_node.find(bn) == offset_by_node.end())
                offset_by_node[bn] = bone->mOffsetMatrix;
        }
    }

    for (uint32_t ji = 0; ji < num_joints; ++ji) {
        const aiNode *jnode = sb->joint_nodes[ji];
        JceJoint *j = &joints[ji];

        /* Name. */
        if (jnode->mName.length > 0) {
            size_t len = jnode->mName.length;
            if (len >= sizeof(j->name)) len = sizeof(j->name) - 1;
            memcpy(j->name, jnode->mName.C_Str(), len);
            j->name[len] = '\0';
        } else {
            snprintf(j->name, sizeof(j->name), "joint_%u", ji);
        }

        /* Parent index: nearest ancestor that is also in the joint set. */
        j->parent = -1;
        const aiNode *p = jnode->mParent;
        while (p) {
            auto it = sb->node_to_joint.find(p);
            if (it != sb->node_to_joint.end()) {
                j->parent = (int16_t)it->second;
                break;
            }
            p = p->mParent;
        }

        /* Inverse bind matrix (assimp row-major -> column-major, same helper as
         * the static path). */
        auto oit = offset_by_node.find(jnode);
        if (oit != offset_by_node.end())
            ai_to_colmajor(oit->second, j->inverse_bind_matrix.raw[0]);
        else
            j->inverse_bind_matrix = jce_m4_identity();

        /* Rest local transform + TRS, from the node's local transform. */
        ai_to_colmajor(jnode->mTransformation, j->local_transform.raw[0]);

        aiVector3D   t, s;
        aiQuaternion q;
        jnode->mTransformation.Decompose(s, q, t);
        j->rest_translation = jce_v3(t.x, t.y, t.z);
        j->rest_rotation.x  = q.x;
        j->rest_rotation.y  = q.y;
        j->rest_rotation.z  = q.z;
        j->rest_rotation.w  = q.w;
        j->rest_scale       = jce_v3(s.x, s.y, s.z);
    }

    JceSkeleton *skel = jce_skeleton_create(joints, num_joints);
    JCE_FREE(joints);   /* jce_skeleton_create copied the joints */

    /* Root (armature) world transform: the world transform of the node ABOVE
     * the first root joint, mirroring extract_skeleton's armature bake so any
     * model-level orientation/scale carried on the armature node applies to the
     * whole skeleton.  Derived entirely from `sb` (joints[] is freed above) to
     * avoid a use-after-free. */
    if (skel) {
        const aiNode *armature = nullptr;
        for (uint32_t ji = 0; ji < num_joints; ++ji) {
            const aiNode *jn = sb->joint_nodes[ji];
            bool is_root = true;
            const aiNode *pp = jn->mParent;
            while (pp) {
                if (sb->node_to_joint.find(pp) != sb->node_to_joint.end())
                    { is_root = false; break; }
                pp = pp->mParent;
            }
            if (is_root) { armature = jn->mParent; break; }
        }
        if (armature) {
            aiMatrix4x4 world;     /* identity */
            const aiNode *n = armature;
            /* accumulate ancestor chain: world = root * ... * armature */
            std::vector<const aiNode *> chain;
            while (n) { chain.push_back(n); n = n->mParent; }
            for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit)
                world = world * (*rit)->mTransformation;
            jce_mat4 rootx;
            ai_to_colmajor(world, rootx.raw[0]);
            jce_skeleton_set_root_transform(skel, &rootx);
        }
    }

    return skel;
}

/* One vertex's accumulated bone influences before top-4 selection. */
struct VertexInfluence {
    int   joint;
    float weight;
};

/* Build skinned vertices for one mesh: pos/normal/uv/tangent + bone
 * indices[4]/weights[4].  Inverts each bone's vertex weight list into per-vertex
 * influences, keeps the top-4 by weight, normalizes to sum 1.0, and maps bone
 * name -> joint index.  Returns a JCE_MALLOC'd array (caller frees) or nullptr. */
static JceSkinnedVertex *build_skinned_vertices(const aiMesh *mesh,
                                                const SkinBuild *sb,
                                                uint32_t *out_count)
{
    *out_count = 0;
    if (!mesh || mesh->mNumVertices == 0) return nullptr;

    uint32_t nv = mesh->mNumVertices;
    JceSkinnedVertex *verts =
        (JceSkinnedVertex *)JCE_CALLOC(nv, sizeof(JceSkinnedVertex));
    if (!verts) return nullptr;

    /* Geometry (mirrors the static path; tangents from CalcTangentSpace). */
    for (uint32_t i = 0; i < nv; ++i) {
        JceSkinnedVertex *v = &verts[i];
        v->pos[0] = mesh->mVertices[i].x;
        v->pos[1] = mesh->mVertices[i].y;
        v->pos[2] = mesh->mVertices[i].z;

        if (mesh->mNormals) {
            v->normal[0] = mesh->mNormals[i].x;
            v->normal[1] = mesh->mNormals[i].y;
            v->normal[2] = mesh->mNormals[i].z;
        } else {
            v->normal[1] = 1.0f;
        }

        if (mesh->mTextureCoords[0]) {
            v->uv[0] = mesh->mTextureCoords[0][i].x;
            v->uv[1] = mesh->mTextureCoords[0][i].y;
        }

        if (mesh->mTangents) {
            v->tangent[0] = mesh->mTangents[i].x;
            v->tangent[1] = mesh->mTangents[i].y;
            v->tangent[2] = mesh->mTangents[i].z;
            /* Handedness from bitangent = sign(cross(N,T) . B). */
            float w = 1.0f;
            if (mesh->mBitangents) {
                aiVector3D N = mesh->mNormals ? mesh->mNormals[i]
                                              : aiVector3D(0, 1, 0);
                aiVector3D T = mesh->mTangents[i];
                aiVector3D B = mesh->mBitangents[i];
                aiVector3D NxT = N ^ T;            /* cross */
                if ((NxT * B) < 0.0f) w = -1.0f;   /* dot */
            }
            v->tangent[3] = w;
        } else {
            v->tangent[0] = 1.0f;
            v->tangent[3] = 1.0f;
        }
    }

    /* Per-vertex influence accumulation (bone -> vertex inverted to
     * vertex -> bones). */
    std::vector<std::vector<VertexInfluence>> infl(nv);
    for (unsigned b = 0; b < mesh->mNumBones; ++b) {
        const aiBone *bone = mesh->mBones[b];
        if (!bone) continue;
        auto jit = sb->name_to_joint.find(bone->mName.C_Str());
        if (jit == sb->name_to_joint.end()) continue;  /* bone clamped out */
        int joint = jit->second;
        for (unsigned w = 0; w < bone->mNumWeights; ++w) {
            const aiVertexWeight &vw = bone->mWeights[w];
            if (vw.mVertexId >= nv || vw.mWeight <= 0.0f) continue;
            infl[vw.mVertexId].push_back({ joint, vw.mWeight });
        }
    }

    /* Top-4 by weight, normalize to sum 1.0. */
    for (uint32_t i = 0; i < nv; ++i) {
        std::vector<VertexInfluence> &vi = infl[i];
        /* selection sort for the top 4 (lists are short after LimitBoneWeights) */
        size_t keep = vi.size() < JCE_MAX_BONE_INFLUENCES
                          ? vi.size() : (size_t)JCE_MAX_BONE_INFLUENCES;
        for (size_t a = 0; a < keep; ++a) {
            size_t best = a;
            for (size_t c = a + 1; c < vi.size(); ++c)
                if (vi[c].weight > vi[best].weight) best = c;
            if (best != a) std::swap(vi[a], vi[best]);
        }

        float sum = 0.0f;
        for (size_t a = 0; a < keep; ++a) sum += vi[a].weight;

        JceSkinnedVertex *v = &verts[i];
        if (keep == 0 || sum <= 0.0f) {
            /* Unweighted vertex -> bind fully to joint 0 so it is not collapsed
             * to the origin (matches assimp's "rigid bind" fallback). */
            v->joints[0]  = 0;
            v->weights[0] = 1.0f;
            continue;
        }
        for (size_t a = 0; a < keep; ++a) {
            int joint = vi[a].joint;
            if (joint < 0) joint = 0;
            if (joint > 255) joint = 255;  /* uint8 ceiling (JCE_MAX_BONES<=128) */
            v->joints[a]  = (uint8_t)joint;
            v->weights[a] = vi[a].weight / sum;
        }
    }

    *out_count = nv;
    return verts;
}

/* Read one mesh's triangle indices into a JCE_MALLOC'd uint32 array (local to
 * the mesh — one node = one mesh, no merge, so no vertex offset). */
static uint32_t *build_mesh_indices(const aiMesh *mesh, uint32_t *out_count)
{
    *out_count = 0;
    if (!mesh) return nullptr;
    uint32_t tris = 0;
    for (unsigned f = 0; f < mesh->mNumFaces; ++f)
        if (mesh->mFaces[f].mNumIndices == 3) tris++;
    if (tris == 0) return nullptr;

    uint32_t *idx = (uint32_t *)JCE_MALLOC((size_t)tris * 3 * sizeof(uint32_t));
    if (!idx) return nullptr;
    uint32_t n = 0;
    for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        if (face.mNumIndices != 3) continue;
        idx[n++] = face.mIndices[0];
        idx[n++] = face.mIndices[1];
        idx[n++] = face.mIndices[2];
    }
    *out_count = n;
    return idx;
}

/* Build all animation clips (mirrors extract_animations).  Each aiAnimation ->
 * one JceAnimClip; each aiNodeAnim channel -> up to three JceAnimChannels
 * (translation / rotation / scale).  Key times convert ticks -> seconds via
 * mTicksPerSecond.  Returns a JCE_MALLOC'd array of owned clips (caller / the
 * builder owns) or nullptr. */
static JceAnimClip **build_anim_clips(const aiScene *scene, const SkinBuild *sb,
                                      uint32_t *out_count)
{
    *out_count = 0;
    if (!scene || scene->mNumAnimations == 0) return nullptr;

    uint32_t num_anims = scene->mNumAnimations;
    JceAnimClip **clips =
        (JceAnimClip **)JCE_CALLOC(num_anims, sizeof(JceAnimClip *));
    if (!clips) return nullptr;

    for (uint32_t ai = 0; ai < num_anims; ++ai) {
        const aiAnimation *anim = scene->mAnimations[ai];
        double tps = (anim->mTicksPerSecond > 0.0) ? anim->mTicksPerSecond : 25.0;

        /* Up to 3 channels (T/R/S) per node-anim channel. */
        uint32_t cap = anim->mNumChannels * 3u;
        JceAnimChannel *channels =
            (JceAnimChannel *)JCE_CALLOC(cap ? cap : 1, sizeof(JceAnimChannel));
        if (!channels) continue;

        float duration = 0.0f;
        uint32_t nch = 0;

        for (unsigned ci = 0; ci < anim->mNumChannels; ++ci) {
            const aiNodeAnim *na = anim->mChannels[ci];
            if (!na) continue;
            auto jit = sb->name_to_joint.find(na->mNodeName.C_Str());
            if (jit == sb->name_to_joint.end()) continue;  /* not a skel joint */
            uint32_t joint = (uint32_t)jit->second;

            /* Translation. */
            if (na->mNumPositionKeys > 0) {
                JceAnimChannel *dst = &channels[nch];
                dst->joint_index   = joint;
                dst->target        = JCE_ANIM_TARGET_TRANSLATION;
                dst->interpolation = JCE_INTERP_LINEAR;
                dst->count         = na->mNumPositionKeys;
                dst->timestamps    = (float *)JCE_MALLOC(
                    (size_t)dst->count * sizeof(float));
                dst->translations  = (jce_vec3 *)JCE_MALLOC(
                    (size_t)dst->count * sizeof(jce_vec3));
                if (!dst->timestamps || !dst->translations) {
                    JCE_FREE(dst->timestamps); JCE_FREE(dst->translations);
                    dst->timestamps = nullptr; dst->translations = nullptr;
                } else {
                    for (uint32_t k = 0; k < dst->count; ++k) {
                        float t = (float)(na->mPositionKeys[k].mTime / tps);
                        dst->timestamps[k] = t;
                        if (t > duration) duration = t;
                        const aiVector3D &val = na->mPositionKeys[k].mValue;
                        dst->translations[k] = jce_v3(val.x, val.y, val.z);
                    }
                    nch++;
                }
            }

            /* Rotation. */
            if (na->mNumRotationKeys > 0) {
                JceAnimChannel *dst = &channels[nch];
                dst->joint_index   = joint;
                dst->target        = JCE_ANIM_TARGET_ROTATION;
                dst->interpolation = JCE_INTERP_LINEAR;
                dst->count         = na->mNumRotationKeys;
                dst->timestamps    = (float *)JCE_MALLOC(
                    (size_t)dst->count * sizeof(float));
                dst->rotations     = (jce_quat *)JCE_MALLOC(
                    (size_t)dst->count * sizeof(jce_quat));
                if (!dst->timestamps || !dst->rotations) {
                    JCE_FREE(dst->timestamps); JCE_FREE(dst->rotations);
                    dst->timestamps = nullptr; dst->rotations = nullptr;
                } else {
                    for (uint32_t k = 0; k < dst->count; ++k) {
                        float t = (float)(na->mRotationKeys[k].mTime / tps);
                        dst->timestamps[k] = t;
                        if (t > duration) duration = t;
                        const aiQuaternion &q = na->mRotationKeys[k].mValue;
                        dst->rotations[k].x = q.x;
                        dst->rotations[k].y = q.y;
                        dst->rotations[k].z = q.z;
                        dst->rotations[k].w = q.w;
                    }
                    nch++;
                }
            }

            /* Scale. */
            if (na->mNumScalingKeys > 0) {
                JceAnimChannel *dst = &channels[nch];
                dst->joint_index   = joint;
                dst->target        = JCE_ANIM_TARGET_SCALE;
                dst->interpolation = JCE_INTERP_LINEAR;
                dst->count         = na->mNumScalingKeys;
                dst->timestamps    = (float *)JCE_MALLOC(
                    (size_t)dst->count * sizeof(float));
                dst->scales        = (jce_vec3 *)JCE_MALLOC(
                    (size_t)dst->count * sizeof(jce_vec3));
                if (!dst->timestamps || !dst->scales) {
                    JCE_FREE(dst->timestamps); JCE_FREE(dst->scales);
                    dst->timestamps = nullptr; dst->scales = nullptr;
                } else {
                    for (uint32_t k = 0; k < dst->count; ++k) {
                        float t = (float)(na->mScalingKeys[k].mTime / tps);
                        dst->timestamps[k] = t;
                        if (t > duration) duration = t;
                        const aiVector3D &val = na->mScalingKeys[k].mValue;
                        dst->scales[k] = jce_v3(val.x, val.y, val.z);
                    }
                    nch++;
                }
            }
        }

        /* Optional lossy keyframe reduction (opt-in import setting; default OFF
         * -> byte-identical).  All assimp tracks are LINEAR. */
        if (jce_anim_compress_is_enabled()) {
            JceAnimCompressParams cp;
            jce_anim_compress_get_params(&cp);
            for (uint32_t c = 0; c < nch; ++c) {
                JceAnimChannel *ch = &channels[c];
                if (!ch->timestamps) continue;
                if (ch->target == JCE_ANIM_TARGET_ROTATION) {
                    if (ch->rotations)
                        ch->count = jce_anim_compress_track(ch->timestamps, ch->rotations,
                            ch->count, JCE_ANIM_COMPRESS_QUAT, &cp);
                } else {
                    jce_vec3 *vals = (ch->target == JCE_ANIM_TARGET_TRANSLATION)
                                         ? ch->translations : ch->scales;
                    if (vals)
                        ch->count = jce_anim_compress_track(ch->timestamps, vals,
                            ch->count, JCE_ANIM_COMPRESS_VEC3, &cp);
                }
            }
        }

        const char *name = (anim->mName.length > 0) ? anim->mName.C_Str()
                                                    : "unnamed";
        clips[ai] = jce_anim_clip_create(name, channels, nch, duration);

        /* Free the temporary channel data (jce_anim_clip_create copied it). */
        for (uint32_t c = 0; c < nch; ++c) {
            JCE_FREE(channels[c].timestamps);
            JCE_FREE(channels[c].translations);
            JCE_FREE(channels[c].rotations);
            JCE_FREE(channels[c].scales);
        }
        JCE_FREE(channels);
    }

    *out_count = num_anims;
    return clips;
}

/* Assemble the JceModelCpu from a parsed skinned aiScene via the renderer's
 * CPU-model builder.  One mesh -> one builder node (no PreTransformVertices, so
 * the geometry is bone-space / node-local and the skinning palette places it).
 * Returns nullptr on failure (caller falls back to the static path). */
static JceModelCpu *build_skinned_model_cpu(const aiScene *scene)
{
    SkinBuild sb;
    if (!build_skin_joint_set(scene, &sb)) {
        LOG_WARN(LOG_TAG, "skinned import: could not resolve a joint set");
        return nullptr;
    }

    JceSkeleton *skel = build_skeleton(scene, &sb);
    if (!skel) return nullptr;

    JceModelCpu *cpu = jce_model_cpu_builder_create();
    if (!cpu) { jce_skeleton_destroy(skel); return nullptr; }

    /* Count meshes that carry geometry. */
    uint32_t mesh_count = 0;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m)
        if (scene->mMeshes[m] && scene->mMeshes[m]->mNumVertices > 0)
            mesh_count++;
    if (mesh_count == 0) {
        jce_skeleton_destroy(skel);
        jce_gltf_model_cpu_free(cpu);
        return nullptr;
    }

    if (!jce_model_cpu_builder_reserve_nodes(cpu, mesh_count) ||
        !jce_model_cpu_builder_set_default_material(cpu)) {
        jce_skeleton_destroy(skel);
        jce_gltf_model_cpu_free(cpu);
        return nullptr;
    }

    uint32_t node_idx = 0;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh *mesh = scene->mMeshes[m];
        if (!mesh || mesh->mNumVertices == 0) continue;

        uint32_t vcount = 0, icount = 0;
        JceSkinnedVertex *verts = build_skinned_vertices(mesh, &sb, &vcount);
        uint32_t *indices = build_mesh_indices(mesh, &icount);

        if (verts && vcount > 0) {
            const char *nm = (mesh->mName.length > 0) ? mesh->mName.C_Str()
                                                      : "skinned_mesh";
            /* No node transform: skinned vertices are placed by the bone
             * palette, so the model-to-world matrix is supplied at draw time. */
            jce_model_cpu_builder_set_skinned_node(
                cpu, node_idx, nm, nullptr, verts, vcount,
                indices, icount, 0u);
            node_idx++;
        }
        JCE_FREE(verts);
        JCE_FREE(indices);
    }

    /* Skeleton + animations move into the model. */
    jce_model_cpu_builder_set_skeleton(cpu, skel);

    uint32_t num_anims = 0;
    JceAnimClip **clips = build_anim_clips(scene, &sb, &num_anims);
    if (clips) jce_model_cpu_builder_set_anims(cpu, clips, num_anims);

    LOG_DEBUG(LOG_TAG,
              "skinned import: %u joints, %u mesh node(s), %u anim clip(s)",
              jce_skeleton_joint_count(skel), node_idx, num_anims);
    return cpu;
}

/* Assemble an "animation library" JceModelCpu: a skeleton built from the
 * animated node hierarchy + the anim clips, with ZERO mesh nodes.  Used for the
 * separate per-clip animation FBX workflow (no skinned mesh in the file).  The
 * CPU-model builder + jce_gltf_upload_cpu already tolerate zero mesh nodes
 * (jce_model_cpu_builder_reserve_nodes(0) returns true; jce_gltf_upload_cpu
 * guards num_nodes>0), so the resulting model is a valid skeleton+clips-only
 * retarget / animation source.  Returns nullptr if no joint set or no clips
 * could be built. */
static JceModelCpu *build_anim_only_model_cpu(const aiScene *scene)
{
    SkinBuild sb;
    if (!build_anim_joint_set(scene, &sb)) {
        LOG_WARN(LOG_TAG,
                 "anim-only import: could not resolve a joint set from channels");
        return nullptr;
    }

    JceSkeleton *skel = build_skeleton(scene, &sb);
    if (!skel) return nullptr;

    uint32_t num_anims = 0;
    JceAnimClip **clips = build_anim_clips(scene, &sb, &num_anims);
    if (!clips || num_anims == 0) {
        if (clips) JCE_FREE(clips);
        jce_skeleton_destroy(skel);
        LOG_WARN(LOG_TAG, "anim-only import: no animation clips extracted");
        return nullptr;
    }

    JceModelCpu *cpu = jce_model_cpu_builder_create();
    if (!cpu) {
        for (uint32_t i = 0; i < num_anims; ++i) jce_anim_clip_destroy(clips[i]);
        JCE_FREE(clips);
        jce_skeleton_destroy(skel);
        return nullptr;
    }

    /* Zero mesh nodes (reserve_nodes(0) returns true) + a default material so the
     * upload path always has at least one material even though no prim uses it. */
    if (!jce_model_cpu_builder_reserve_nodes(cpu, 0) ||
        !jce_model_cpu_builder_set_default_material(cpu)) {
        for (uint32_t i = 0; i < num_anims; ++i) jce_anim_clip_destroy(clips[i]);
        JCE_FREE(clips);
        jce_skeleton_destroy(skel);
        jce_gltf_model_cpu_free(cpu);
        return nullptr;
    }

    jce_model_cpu_builder_set_skeleton(cpu, skel);
    jce_model_cpu_builder_set_anims(cpu, clips, num_anims);

    LOG_DEBUG(LOG_TAG,
              "anim-only import: %u joints, 0 mesh node(s), %u anim clip(s)",
              jce_skeleton_joint_count(skel), num_anims);
    return cpu;
}

/* Public: decode skinned model from memory (bgfx-free).  hint is the file
 * extension (".fbx" etc.) for assimp's format detection.  Returns a JceModelCpu
 * (skinned if the scene has bones, an animation-library skeleton+clips model if
 * the scene is animation-only, else a static single-primitive fallback) or
 * nullptr on parse failure. */
static JceModelCpu *decode_skinned_memory(const void *data, size_t size,
                                          const char *hint)
{
    if (!data || size == 0) return nullptr;

    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFileFromMemory(
        data, size, skinned_postprocess_flags(hint), hint ? hint : "");

    /* RETRY: the geometry-heavy post-processes can make assimp return NULL on an
     * animation-only FBX (no renderable mesh).  Retry with the minimal flag set
     * (GlobalScale + LimitBoneWeights + PopulateArmatureData only) before giving
     * up — this is what enables the separate per-clip animation FBX workflow. */
    if (!scene) {
        LOG_INFO(LOG_TAG,
                 "skinned import: full-flag parse failed (%s); retrying with "
                 "minimal flags (anim-only FBX path)",
                 importer.GetErrorString());
        scene = importer.ReadFileFromMemory(
            data, size, skinned_minimal_flags(hint), hint ? hint : "");
    }

    if (!scene) {
        LOG_ERROR(LOG_TAG, "skinned import: assimp parse failed: %s",
                  importer.GetErrorString());
        return nullptr;
    }

    /* Animation-only scene: has clips but no skinned mesh (possibly no meshes at
     * all).  Build a skeleton+clips "animation library" model (zero mesh nodes).
     * Checked before the mNumMeshes==0 bail so an anim-only file still decodes. */
    if (scene->mNumAnimations > 0 && !scene_is_skinned(scene)) {
        JceModelCpu *anim_cpu = build_anim_only_model_cpu(scene);
        if (anim_cpu) return anim_cpu;
        /* Fall through: if no joint set / clips resolved, try the mesh paths
         * below (covers the rare "anim channels present but unresolvable + a
         * static mesh also present" case). */
        LOG_WARN(LOG_TAG,
                 "skinned import: anim-only extraction failed, trying mesh path");
    }

    if (scene->mNumMeshes == 0) {
        LOG_ERROR(LOG_TAG,
                  "skinned import: scene has no meshes and no usable animation");
        return nullptr;
    }

    if (scene_is_skinned(scene)) {
        JceModelCpu *cpu = build_skinned_model_cpu(scene);
        if (cpu) return cpu;
        /* Fall through to the static fallback on extraction failure. */
        LOG_WARN(LOG_TAG, "skinned import: extraction failed, static fallback");
    }

    /* Non-skinned (or extraction failed): emit a single static-PBR node so the
     * SAME JceModel type is returned.  Merge all meshes' geometry into one node
     * (mirrors convert_all_meshes' merge), keeping tangents. */
    uint32_t total_verts = 0, total_tris = 0;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh *ai = scene->mMeshes[m];
        if (!ai) continue;
        total_verts += ai->mNumVertices;
        for (unsigned f = 0; f < ai->mNumFaces; ++f)
            if (ai->mFaces[f].mNumIndices == 3) total_tris++;
    }
    if (total_verts == 0) return nullptr;

    JcePbrVertex *verts =
        (JcePbrVertex *)JCE_CALLOC(total_verts, sizeof(JcePbrVertex));
    uint32_t *indices = (total_tris > 0)
        ? (uint32_t *)JCE_MALLOC((size_t)total_tris * 3 * sizeof(uint32_t))
        : nullptr;
    if (!verts || (total_tris > 0 && !indices)) {
        JCE_FREE(verts); JCE_FREE(indices);
        return nullptr;
    }

    uint32_t voff = 0, icnt = 0;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh *ai = scene->mMeshes[m];
        if (!ai) continue;
        for (unsigned i = 0; i < ai->mNumVertices; ++i) {
            JcePbrVertex *v = &verts[voff + i];
            v->pos[0] = ai->mVertices[i].x;
            v->pos[1] = ai->mVertices[i].y;
            v->pos[2] = ai->mVertices[i].z;
            if (ai->mNormals) {
                v->normal[0] = ai->mNormals[i].x;
                v->normal[1] = ai->mNormals[i].y;
                v->normal[2] = ai->mNormals[i].z;
            } else { v->normal[1] = 1.0f; }
            if (ai->mTextureCoords[0]) {
                v->uv[0] = ai->mTextureCoords[0][i].x;
                v->uv[1] = ai->mTextureCoords[0][i].y;
            }
            if (ai->mTangents) {
                v->tangent[0] = ai->mTangents[i].x;
                v->tangent[1] = ai->mTangents[i].y;
                v->tangent[2] = ai->mTangents[i].z;
                v->tangent[3] = 1.0f;
            } else { v->tangent[0] = 1.0f; v->tangent[3] = 1.0f; }
        }
        for (unsigned f = 0; f < ai->mNumFaces; ++f) {
            const aiFace &face = ai->mFaces[f];
            if (face.mNumIndices != 3) continue;
            indices[icnt++] = face.mIndices[0] + voff;
            indices[icnt++] = face.mIndices[1] + voff;
            indices[icnt++] = face.mIndices[2] + voff;
        }
        voff += ai->mNumVertices;
    }

    JceModelCpu *cpu = jce_model_cpu_builder_create();
    if (!cpu) { JCE_FREE(verts); JCE_FREE(indices); return nullptr; }
    if (!jce_model_cpu_builder_reserve_nodes(cpu, 1) ||
        !jce_model_cpu_builder_set_default_material(cpu)) {
        jce_gltf_model_cpu_free(cpu);
        JCE_FREE(verts); JCE_FREE(indices);
        return nullptr;
    }
    jce_model_cpu_builder_set_static_node(cpu, 0, "mesh", nullptr,
                                          verts, total_verts,
                                          indices, icnt, 0u);
    JCE_FREE(verts);
    JCE_FREE(indices);
    return cpu;
}

JceModelCpu *jce_model_importer_decode_skinned_pak(const JcePakArchive *pak,
                                                   const char *asset_path)
{
    if (!pak || !asset_path) return nullptr;

    const JcePakAsset *asset = jce_pak_find(pak, asset_path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "skinned import: model not found in PAK: %s",
                  asset_path);
        return nullptr;
    }
    void *buf = JCE_MALLOC(static_cast<size_t>(asset->original_size));
    if (!buf) return nullptr;
    size_t n = jce_pak_decompress(asset, buf,
                                  static_cast<size_t>(asset->original_size));
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "skinned import: decompression failed: %s",
                  asset_path);
        JCE_FREE(buf);
        return nullptr;
    }
    const char *ext = "";
    const char *dot = strrchr(asset_path, '.');
    if (dot) ext = dot;
    JceModelCpu *cpu = decode_skinned_memory(buf, n, ext);
    JCE_FREE(buf);
    return cpu;
}

JceModelCpu *jce_model_importer_decode_skinned_file(const char *file_path)
{
    if (!file_path || file_path[0] == '\0') return nullptr;

    uint64_t vsz = 0;
    void *vbuf = jce_fs_host_read_all(file_path, &vsz);
    if (!vbuf || vsz == 0) {
        if (vbuf) jce_fs_buffer_free(vbuf);
        LOG_ERROR(LOG_TAG, "skinned import: cannot read file: %s", file_path);
        return nullptr;
    }
    const char *ext = "";
    const char *dot = strrchr(file_path, '.');
    if (dot) ext = dot;
    JceModelCpu *cpu = decode_skinned_memory(vbuf, (size_t)vsz, ext);
    jce_fs_buffer_free(vbuf);
    return cpu;
}

JceModel *jce_model_importer_load_skinned_pak(const JcePakArchive *pak,
                                              const char *asset_path)
{
    /* decode (no bgfx) + upload (render thread, bgfx).  Must be on the render
     * thread because jce_model_upload_gltf_cpu creates GPU buffers. */
    return jce_model_upload_gltf_cpu(
        jce_model_importer_decode_skinned_pak(pak, asset_path));
}

JceModel *jce_model_importer_load_skinned_file(const char *file_path)
{
    return jce_model_upload_gltf_cpu(
        jce_model_importer_decode_skinned_file(file_path));
}
#endif /* !JCE_MODEL_IMPORTER_COOK_ONLY */

/* ── Material extraction ─────────────────────────────────────────── */

static constexpr unsigned ASSIMP_FLAGS =
    aiProcess_Triangulate | aiProcess_GenSmoothNormals
    | aiProcess_FlipUVs   | aiProcess_CalcTangentSpace;

/*
 * Resolve a texture path obtained from assimp relative to the model's
 * parent directory.  If the path starts with '*' (embedded texture), or
 * points to an already-absolute path, it is kept as-is.  Otherwise, the
 * relative URI is joined with the model's directory.
 */
static void resolve_tex_path(const char *model_path,
                              const aiString &ai_path,
                              char *out, size_t out_size)
{
    out[0] = '\0';
    if (ai_path.length == 0) return;

    const char *raw = ai_path.C_Str();

    /* Embedded texture reference (e.g. "*0") — handled separately. */
    if (raw[0] == '*') {
        snprintf(out, out_size, "%s", raw);
        return;
    }

    /* Extract the model's parent directory for later fallback. */
    std::string parent_dir;
    {
        std::string mp(model_path);
        size_t sep = mp.find_last_of("/\\");
        if (sep != std::string::npos)
            parent_dir = mp.substr(0, sep + 1);
    }

    /* Already absolute? */
    bool is_absolute = (raw[0] == '/' || raw[0] == '\\'
                        || (raw[0] != '\0' && raw[1] == ':'));
    if (is_absolute) {
        /* If it exists, use it (converted to CWD-relative). */
        if (fs::exists(raw)) {
            std::string rel = to_cwd_relative(raw);
            snprintf(out, out_size, "%s", rel.c_str());
            return;
        }
        /* FBX often embeds the original creator's absolute path which
         * doesn't exist on this machine.  Fall back to just the filename
         * resolved against the model's directory. */
        std::string fname = fs::path(raw).filename().string();
        if (!fname.empty() && !parent_dir.empty()) {
            std::string fallback = parent_dir + fname;
            if (fs::exists(fallback)) {
                std::string rel = to_cwd_relative(fallback);
                snprintf(out, out_size, "%s", rel.c_str());
                return;
            }
        }
        /* Path doesn't exist anywhere — store nothing. */
        return;
    }

    /* Relative URI: parent_dir(model_path) / relative_uri */
    std::string resolved = parent_dir + raw;
    if (fs::exists(resolved)) {
        std::string rel = to_cwd_relative(resolved);
        snprintf(out, out_size, "%s", rel.c_str());
        return;
    }
    /* Fallback: just the filename in the model directory. */
    std::string fname = fs::path(raw).filename().string();
    if (!fname.empty() && !parent_dir.empty()) {
        std::string fallback = parent_dir + fname;
        if (fs::exists(fallback)) {
            std::string rel = to_cwd_relative(fallback);
            snprintf(out, out_size, "%s", rel.c_str());
            return;
        }
    }
    /* Store the best guess (CWD-relative) even if it doesn't exist. */
    std::string rel = to_cwd_relative(resolved);
    snprintf(out, out_size, "%s", rel.c_str());
}

/*
 * Try to extract a texture path of the given type from the material.
 * Returns true if a non-empty path was found.
 */
static bool extract_tex(const aiMaterial *mat,
                        aiTextureType type, unsigned index,
                        const char *model_path,
                        char *out, size_t out_size)
{
    aiString ai_path;
    if (mat->GetTexture(type, index, &ai_path) != AI_SUCCESS)
        return false;
    if (ai_path.length == 0) return false;
    resolve_tex_path(model_path, ai_path, out, out_size);
    return out[0] != '\0';
}

/*
 * Write an embedded assimp texture to disk next to the model file.
 * Returns true and fills out_path on success.
 */
static bool write_embedded_texture(const aiTexture *tex,
                                   const char *model_path,
                                   int tex_index,
                                   char *out_path, size_t out_size)
{
    if (!tex) return false;

    /* Derive output filename: <model_stem>_tex<index>.<hint> */
    std::string mp(model_path);
    size_t sep = mp.find_last_of("/\\");
    std::string dir  = (sep != std::string::npos) ? mp.substr(0, sep + 1) : "";
    std::string stem = (sep != std::string::npos) ? mp.substr(sep + 1) : mp;
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);

    const char *ext = (tex->achFormatHint[0] != '\0') ? tex->achFormatHint : "png";
    char fname[256];
    snprintf(fname, sizeof(fname), "%s_tex%d.%s", stem.c_str(), tex_index, ext);

    std::string full = dir + fname;

    if (tex->mHeight == 0) {
        /* Compressed data (e.g. PNG/JPG stored as-is). */
        if (!jce_fs_host_write_all(full.c_str(), tex->pcData, tex->mWidth))
            return false;
    } else {
        /* Uncompressed ARGB8888 — write as raw RGBA TGA for simplicity.
         * The texture cache can load TGA natively via SDL_image. */
        uint32_t w = tex->mWidth, h = tex->mHeight;
        size_t pixel_count = (size_t)w * h;

        uint8_t tga_header[18] = {};
        tga_header[2]  = 2; /* uncompressed true-color */
        tga_header[12] = (uint8_t)(w & 0xFF);
        tga_header[13] = (uint8_t)((w >> 8) & 0xFF);
        tga_header[14] = (uint8_t)(h & 0xFF);
        tga_header[15] = (uint8_t)((h >> 8) & 0xFF);
        tga_header[16] = 32; /* bits per pixel */
        tga_header[17] = 0x20; /* top-left origin */

        /* Override extension to tga. */
        snprintf(fname, sizeof(fname), "%s_tex%d.tga", stem.c_str(), tex_index);
        full = dir + fname;

        size_t total = sizeof(tga_header) + pixel_count * 4;
        uint8_t *blob = (uint8_t *)JCE_MALLOC(total);
        if (!blob) return false;
        memcpy(blob, tga_header, sizeof(tga_header));

        /* assimp stores ARGB8888; TGA expects BGRA. */
        const aiTexel *src = tex->pcData;
        uint8_t *dst = blob + sizeof(tga_header);
        for (size_t i = 0; i < pixel_count; i++) {
            dst[i * 4 + 0] = src[i].b;
            dst[i * 4 + 1] = src[i].g;
            dst[i * 4 + 2] = src[i].r;
            dst[i * 4 + 3] = src[i].a;
        }

        bool ok = jce_fs_host_write_all(full.c_str(), blob, total);
        JCE_FREE(blob);
        if (!ok) return false;
    }

    std::string rel_full = to_cwd_relative(full);
    snprintf(out_path, out_size, "%s", rel_full.c_str());
    LOG_DEBUG(LOG_TAG, "wrote embedded texture: %s", rel_full.c_str());
    return true;
}

/*
 * Record an extracted texture's colour space beside it, as
 * `<texture>.import.json` with a single "colorSpace" key.
 *
 * "colorSpace" and not the existing "srgb" boolean: that key is written by the
 * editor's import-preset panel, read by nobody, and defaults to TRUE -- so
 * every sidecar already on disk asserts sRGB, normal maps included.  Honouring
 * it would corrupt the data this is meant to protect.  A key with no legacy
 * writers can be trusted the moment it appears.
 *
 * Merges rather than overwrites: a sidecar may already carry target_format /
 * gen_mips / max_size / quality that a human chose, and losing those to record
 * one fact would be a poor trade.  A write failure is not fatal -- the cooker
 * falls back to its filename heuristic, which is where it was before.
 */
static void write_embedded_colour_space(const char *model_path,
                                        const char *tex_rel_path,
                                        bool srgb)
{
    if (!model_path || !tex_rel_path || !tex_rel_path[0]) return;

    /* tex_rel_path is relative to the model; rebuild the host path the same
     * way write_embedded_texture did. */
    std::string mp(model_path);
    size_t sep = mp.find_last_of("/\\");
    std::string dir = (sep != std::string::npos) ? mp.substr(0, sep + 1) : "";
    std::string base(tex_rel_path);
    size_t bsep = base.find_last_of("/\\");
    if (bsep != std::string::npos) base = base.substr(bsep + 1);
    const std::string side = dir + base + ".import.json";

    JceJson *root = NULL;
    /* uint64_t，不是 size_t：jce_fs_host_read_all 的第二参是 uint64_t*。
     * 在 x64 上两者同宽所以过得去，wasm32 的 size_t 只有 32 位，clang 直接
     * 拒绝 —— 这类「只在 32 位目标上暴露」的宽度错配，光在 x64 上构建
     * 永远看不见。 */
    uint64_t existing_len = 0;
    void *existing = jce_fs_host_read_all(side.c_str(), &existing_len);
    if (existing) {
        root = jce_json_parse((const char *)existing, (size_t)existing_len);
        JCE_FREE(existing);
    }
    if (!jce_json_is_object(root)) {
        if (root) jce_json_free(root);
        root = jce_json_object();
    }
    if (!root) return;

    jce_json_remove(root, "colorSpace");
    jce_json_set_string(root, "colorSpace", srgb ? "srgb" : "linear");

    char *text = jce_json_print(root, /*pretty=*/false);
    jce_json_free(root);
    if (!text) return;
    (void)jce_fs_host_write_all(side.c_str(), text, strlen(text));
    jce_json_free_string(text);
}

/*
 * Resolve embedded texture references (paths starting with '*') by
 * extracting them from the scene and writing to disk.
 */
static void resolve_embedded_textures(const aiScene *scene,
                                      const char *model_path,
                                      JceModelMaterialInfo *out)
{
    auto try_resolve = [&](char *path, size_t sz, bool srgb) {
        if (path[0] != '*') return;
        int idx = atoi(path + 1);
        if (idx < 0 || (unsigned)idx >= scene->mNumTextures) {
            path[0] = '\0';
            return;
        }
        char resolved[256];
        if (write_embedded_texture(scene->mTextures[idx], model_path, idx,
                                   resolved, sizeof(resolved))) {
            snprintf(path, sz, "%s", resolved);
            write_embedded_colour_space(model_path, resolved, srgb);
        } else {
            path[0] = '\0';
        }
    };

    /* THIS is where the texture semantic exists, and until now it was thrown
     * away one line later.
     *
     * A GLB's embedded textures come out as `<stem>_tex<N>.png` -- a name that
     * says nothing about what the texture IS, and 114 of this repository's
     * textures have exactly that shape.  Every downstream guess about their
     * colour space (the cooker's filename heuristic, the sRGB bit in
     * JceAssetTexInfo) is guessing about a name that was generated by this
     * function, which at this very call knew the answer: the slot it is
     * filling.  Recording it costs one small sidecar per extracted file.
     *
     * Albedo and emissive are sRGB-encoded colour.  Metallic-roughness,
     * normal and occlusion are linear data and must never be gamma-decoded. */
    try_resolve(out->albedo_tex,   sizeof(out->albedo_tex),   true);
    try_resolve(out->mr_tex,       sizeof(out->mr_tex),       false);
    try_resolve(out->normal_tex,   sizeof(out->normal_tex),   false);
    try_resolve(out->ao_tex,       sizeof(out->ao_tex),       false);
    try_resolve(out->emissive_tex, sizeof(out->emissive_tex), true);
}

static bool jce_model_importer_extract_material_impl(const char *file_path,
                                       JceModelMaterialInfo *out);

bool jce_model_importer_extract_material(const char *file_path,
                                       JceModelMaterialInfo *out)
{
    return mi_guard("extract_material", false, [&] {
        return jce_model_importer_extract_material_impl(file_path, out);
    });
}

static bool jce_model_importer_extract_material_impl(const char *file_path,
                                       JceModelMaterialInfo *out)
{
    if (!file_path || file_path[0] == '\0' || !out) return false;

    memset(out, 0, sizeof(*out));
    out->base_color[0] = 1.0f;
    out->base_color[1] = 1.0f;
    out->base_color[2] = 1.0f;
    out->base_color[3] = 1.0f;
    out->metallic      = 0.0f;
    out->roughness      = 0.5f;
    out->normal_scale   = 1.0f;
    out->ao_strength    = 1.0f;
    out->alpha_cutoff   = 0.5f;

    /* Host path on purpose — NOT the VFS-first idiom load_cpu_file uses.
     * Assimp's ReadFileFromMemory "doesn't handle model formats that spread
     * their data across multiple files ... OBJ ... outsource parts of their
     * material info into external scripts" (Importer.hpp): an OBJ's materials
     * live in a sibling .mtl and a non-GLB glTF's in sibling .bin/image files,
     * so a memory buffer would hand back the default material for exactly the
     * formats this function exists to read.  Sound because this is an
     * authoring-only entry point (see the header): it also probes sibling
     * texture files with fs::exists and WRITES a GLB's embedded textures next
     * to the model — neither is available on a read-only VFS/PAK mount, and
     * packaged content never arrives here (cooked bundles carry materials
     * through jce_bundle_convert_to_glb + the glTF loader). */
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(file_path, ASSIMP_FLAGS);
    if (!scene || !scene->mNumMeshes || !scene->mNumMaterials) {
        LOG_WARN(LOG_TAG, "material extract failed (no scene): %s", file_path);
        return false;
    }

    /* Pick the dominant material — the one used by the mesh with the most
     * vertices, so we get the most representative textures. */
    unsigned best_mat = scene->mMeshes[0]->mMaterialIndex;
    unsigned best_verts = scene->mMeshes[0]->mNumVertices;
    for (unsigned m = 1; m < scene->mNumMeshes; m++) {
        if (scene->mMeshes[m]->mNumVertices > best_verts) {
            best_verts = scene->mMeshes[m]->mNumVertices;
            best_mat = scene->mMeshes[m]->mMaterialIndex;
        }
    }

    if (best_mat >= scene->mNumMaterials) best_mat = 0;
    const aiMaterial *mat = scene->mMaterials[best_mat];

    /* ── Texture paths ──────────────────────────────────────────── */
    /* Albedo / base-color */
    if (!extract_tex(mat, aiTextureType_BASE_COLOR, 0, file_path,
                     out->albedo_tex, sizeof(out->albedo_tex))) {
        extract_tex(mat, aiTextureType_DIFFUSE, 0, file_path,
                    out->albedo_tex, sizeof(out->albedo_tex));
    }

    /* Metallic-roughness (glTF packs this as aiTextureType_UNKNOWN idx 0) */
    if (!extract_tex(mat, aiTextureType_UNKNOWN, 0, file_path,
                     out->mr_tex, sizeof(out->mr_tex))) {
        if (!extract_tex(mat, aiTextureType_METALNESS, 0, file_path,
                         out->mr_tex, sizeof(out->mr_tex))) {
            extract_tex(mat, aiTextureType_DIFFUSE_ROUGHNESS, 0, file_path,
                        out->mr_tex, sizeof(out->mr_tex));
        }
    }

    /* Normal */
    if (!extract_tex(mat, aiTextureType_NORMALS, 0, file_path,
                     out->normal_tex, sizeof(out->normal_tex))) {
        extract_tex(mat, aiTextureType_HEIGHT, 0, file_path,
                    out->normal_tex, sizeof(out->normal_tex));
    }

    /* Ambient occlusion */
    if (!extract_tex(mat, aiTextureType_AMBIENT_OCCLUSION, 0, file_path,
                     out->ao_tex, sizeof(out->ao_tex))) {
        extract_tex(mat, aiTextureType_LIGHTMAP, 0, file_path,
                    out->ao_tex, sizeof(out->ao_tex));
    }

    /* Emissive */
    extract_tex(mat, aiTextureType_EMISSIVE, 0, file_path,
                out->emissive_tex, sizeof(out->emissive_tex));

    /* ── Resolve embedded textures (GLB) ─────────────────────────── */
    resolve_embedded_textures(scene, file_path, out);

    /* ── PBR scalar factors ─────────────────────────────────────── */
    aiColor4D color;
    if (aiGetMaterialColor(mat, AI_MATKEY_COLOR_DIFFUSE, &color) == AI_SUCCESS) {
        out->base_color[0] = color.r;
        out->base_color[1] = color.g;
        out->base_color[2] = color.b;
        out->base_color[3] = color.a;
    }
    if (aiGetMaterialColor(mat, AI_MATKEY_BASE_COLOR, &color) == AI_SUCCESS) {
        out->base_color[0] = color.r;
        out->base_color[1] = color.g;
        out->base_color[2] = color.b;
        out->base_color[3] = color.a;
    }

    float fval;
    if (aiGetMaterialFloat(mat, AI_MATKEY_METALLIC_FACTOR, &fval) == AI_SUCCESS)
        out->metallic = fval;
    if (aiGetMaterialFloat(mat, AI_MATKEY_ROUGHNESS_FACTOR, &fval) == AI_SUCCESS)
        out->roughness = fval;

    aiColor3D emissive;
    if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
        out->emissive[0] = emissive.r;
        out->emissive[1] = emissive.g;
        out->emissive[2] = emissive.b;
    }

    int ival;
    if (aiGetMaterialInteger(mat, AI_MATKEY_TWOSIDED, &ival) == AI_SUCCESS)
        out->double_sided = (ival != 0);

    if (aiGetMaterialFloat(mat, AI_MATKEY_OPACITY, &fval) == AI_SUCCESS) {
        if (fval < 1.0f) {
            out->alpha_mode = 2; /* BLEND */
            out->base_color[3] = fval;
        }
    }

    LOG_DEBUG(LOG_TAG, "extracted material from %s: albedo='%s' normal='%s' "
              "mr='%s' metallic=%.2f roughness=%.2f",
              file_path, out->albedo_tex, out->normal_tex, out->mr_tex,
              out->metallic, out->roughness);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════
 *  INSPECTOR API
 * ══════════════════════════════════════════════════════════════════════ */

static constexpr unsigned INSPECT_FLAGS =
    aiProcess_Triangulate
    | aiProcess_JoinIdenticalVertices
    | aiProcess_GenSmoothNormals
    | aiProcess_ImproveCacheLocality;

static bool fill_inspect(const aiScene *scene, bool want_wireframe,
                         JceModelInspectResult *out)
{
    out->mesh_count     = (int)scene->mNumMeshes;
    out->material_count = (int)scene->mNumMaterials;

    /* Material names. */
    if (scene->mNumMaterials > 0) {
        out->material_names = (char **)JCE_CALLOC(scene->mNumMaterials, sizeof(char *));
        if (!out->material_names) {
            snprintf(out->error, sizeof(out->error), "out of memory");
            return false;
        }
        for (unsigned i = 0; i < scene->mNumMaterials; i++) {
            const aiMaterial *mat = scene->mMaterials[i];
            aiString name;
            char     buf[128];
            if (mat && mat->Get(AI_MATKEY_NAME, name) == AI_SUCCESS && name.length > 0)
                snprintf(buf, sizeof(buf), "%s", name.C_Str());
            else
                snprintf(buf, sizeof(buf), "Material %u", i);
            size_t len           = strlen(buf) + 1;
            char  *copy          = (char *)JCE_MALLOC(len);
            if (copy) memcpy(copy, buf, len);
            out->material_names[i] = copy;
        }
    }

    /* Vertex/face totals + bounds. */
    int total_verts = 0, total_faces = 0;
    float minx =  1e30f, miny =  1e30f, minz =  1e30f;
    float maxx = -1e30f, maxy = -1e30f, maxz = -1e30f;
    for (unsigned m = 0; m < scene->mNumMeshes; m++) {
        const aiMesh *mesh = scene->mMeshes[m];
        if (!mesh) continue;
        total_verts += (int)mesh->mNumVertices;
        total_faces += (int)mesh->mNumFaces;
        for (unsigned v = 0; v < mesh->mNumVertices; v++) {
            float x = mesh->mVertices[v].x;
            float y = mesh->mVertices[v].y;
            float z = mesh->mVertices[v].z;
            if (x < minx) minx = x; if (x > maxx) maxx = x;
            if (y < miny) miny = y; if (y > maxy) maxy = y;
            if (z < minz) minz = z; if (z > maxz) maxz = z;
        }
    }
    out->vertex_count   = total_verts;
    out->face_count     = total_faces;
    out->bounds_min[0]  = minx; out->bounds_min[1] = miny; out->bounds_min[2] = minz;
    out->bounds_max[0]  = maxx; out->bounds_max[1] = maxy; out->bounds_max[2] = maxz;

    if (!want_wireframe || total_verts <= 0)
        return true;

    /* Flat vertex/face arrays. */
    out->vertices_xyz = (float *)JCE_MALLOC((size_t)total_verts * 3 * sizeof(float));
    out->face_sizes   = (int   *)JCE_MALLOC((size_t)total_faces * sizeof(int));
    if (!out->vertices_xyz || !out->face_sizes) {
        snprintf(out->error, sizeof(out->error), "out of memory");
        return false;
    }

    int total_face_idx = 0;
    for (unsigned m = 0; m < scene->mNumMeshes; m++) {
        const aiMesh *mesh = scene->mMeshes[m];
        if (!mesh) continue;
        for (unsigned f = 0; f < mesh->mNumFaces; f++)
            total_face_idx += (int)mesh->mFaces[f].mNumIndices;
    }
    out->face_indices = (int *)JCE_MALLOC((size_t)total_face_idx * sizeof(int));
    if (!out->face_indices && total_face_idx > 0) {
        snprintf(out->error, sizeof(out->error), "out of memory");
        return false;
    }

    int vi = 0, fi = 0, fii = 0, vert_base = 0, face_idx = 0;
    for (unsigned m = 0; m < scene->mNumMeshes; m++) {
        const aiMesh *mesh = scene->mMeshes[m];
        if (!mesh) continue;
        for (unsigned v = 0; v < mesh->mNumVertices; v++) {
            out->vertices_xyz[vi++] = mesh->mVertices[v].x;
            out->vertices_xyz[vi++] = mesh->mVertices[v].y;
            out->vertices_xyz[vi++] = mesh->mVertices[v].z;
        }
        for (unsigned f = 0; f < mesh->mNumFaces; f++) {
            const aiFace &face = mesh->mFaces[f];
            if (face.mNumIndices < 2) {
                out->face_sizes[face_idx++] = 0;
                continue;
            }
            out->face_sizes[face_idx++] = (int)face.mNumIndices;
            for (unsigned k = 0; k < face.mNumIndices; k++)
                out->face_indices[fii++] = (int)face.mIndices[k] + vert_base;
            fi++;
        }
        vert_base += (int)mesh->mNumVertices;
    }
    out->face_indices_count = fii;
    return true;
}

static bool jce_model_importer_inspect_memory_impl(const void *data, size_t size,
                                       const char *ext_hint,
                                       bool        want_wireframe,
                                       JceModelInspectResult *out);

bool jce_model_importer_inspect_memory(const void *data, size_t size,
                                       const char *ext_hint,
                                       bool        want_wireframe,
                                       JceModelInspectResult *out)
{
    return mi_guard("inspect_memory", false, [&] {
        return jce_model_importer_inspect_memory_impl(data, size, ext_hint, want_wireframe, out);
    });
}

static bool jce_model_importer_inspect_memory_impl(const void *data, size_t size,
                                       const char *ext_hint,
                                       bool        want_wireframe,
                                       JceModelInspectResult *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!data || size == 0) {
        snprintf(out->error, sizeof(out->error), "empty model content");
        return false;
    }
    Assimp::Importer imp;
    /* NOTE: no fbx_scale_flags here — INSPECT_FLAGS omits PreTransformVertices,
     * so bounds are local-space and already ignore node transforms; adding
     * GlobalScale would scale the local verts and skew the preview bounds
     * further. Inspector bounds are approximate for node-transformed models. */
    const aiScene *scene = imp.ReadFileFromMemory(data, size, INSPECT_FLAGS,
                                                  ext_hint ? ext_hint : "");
    if (!scene || scene->mNumMeshes == 0) {
        const char *err = imp.GetErrorString();
        snprintf(out->error, sizeof(out->error),
                 "assimp: %s", err && err[0] ? err : "unknown error");
        return false;
    }
    return fill_inspect(scene, want_wireframe, out);
}

static bool jce_model_importer_inspect_file_impl(const char *file_path,
                                     bool        want_wireframe,
                                     JceModelInspectResult *out);

bool jce_model_importer_inspect_file(const char *file_path,
                                     bool        want_wireframe,
                                     JceModelInspectResult *out)
{
    return mi_guard("inspect_file", false, [&] {
        return jce_model_importer_inspect_file_impl(file_path, want_wireframe, out);
    });
}

static bool jce_model_importer_inspect_file_impl(const char *file_path,
                                     bool        want_wireframe,
                                     JceModelInspectResult *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!file_path || !file_path[0]) {
        snprintf(out->error, sizeof(out->error), "empty file path");
        return false;
    }
    /* Host path is deliberate, for the same reason extract_material reads one:
     * ReadFileFromMemory cannot follow an OBJ's .mtl or a glTF's sibling .bin,
     * so this entry reports complete metadata where the _memory twin degrades.
     * A caller holding VFS/PAK/bundle bytes uses _inspect_memory instead —
     * the model viewer tries this first and falls back to it on failure. */
    Assimp::Importer imp;
    const aiScene *scene = imp.ReadFile(file_path, INSPECT_FLAGS);  /* see note above */
    if (!scene || scene->mNumMeshes == 0) {
        const char *err = imp.GetErrorString();
        snprintf(out->error, sizeof(out->error),
                 "assimp: %s", err && err[0] ? err : "unknown error");
        return false;
    }
    return fill_inspect(scene, want_wireframe, out);
}

void jce_model_importer_free_inspect(JceModelInspectResult *r)
{
    if (!r) return;
    if (r->material_names) {
        for (int i = 0; i < r->material_count; i++)
            if (r->material_names[i]) JCE_FREE(r->material_names[i]);
        JCE_FREE(r->material_names);
    }
    if (r->vertices_xyz) JCE_FREE(r->vertices_xyz);
    if (r->face_indices) JCE_FREE(r->face_indices);
    if (r->face_sizes)   JCE_FREE(r->face_sizes);
    r->material_names = nullptr;
    r->vertices_xyz   = nullptr;
    r->face_indices   = nullptr;
    r->face_sizes     = nullptr;
}

} /* extern "C" */