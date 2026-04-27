/*
 * jce_model_importer.cpp  Model loading via assimp (engine layer).
 *
 * Uses assimp's C++ interface to load models from disk or PAK assets.
 * Supports all assimp-supported formats: OBJ, FBX, 3DS, glTF, etc.
 */

#include <jce/resource/jce_model_importer.h>

#include <jce/os/core/jce_filesystem.h>

extern "C" {
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_log.h>
}

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

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

bool jce_model_importer_load_cpu_file(const char *file_path,
                                    JceModelCpuMeshData *out)
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
        | aiProcess_CalcTangentSpace
        | aiProcess_PreTransformVertices);

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
        | aiProcess_PreTransformVertices,
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
        | aiProcess_PreTransformVertices);

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
 * Resolve embedded texture references (paths starting with '*') by
 * extracting them from the scene and writing to disk.
 */
static void resolve_embedded_textures(const aiScene *scene,
                                      const char *model_path,
                                      JceModelMaterialInfo *out)
{
    auto try_resolve = [&](char *path, size_t sz) {
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
        } else {
            path[0] = '\0';
        }
    };

    try_resolve(out->albedo_tex,   sizeof(out->albedo_tex));
    try_resolve(out->mr_tex,       sizeof(out->mr_tex));
    try_resolve(out->normal_tex,   sizeof(out->normal_tex));
    try_resolve(out->ao_tex,       sizeof(out->ao_tex));
    try_resolve(out->emissive_tex, sizeof(out->emissive_tex));
}

bool jce_model_importer_extract_material(const char *file_path,
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

bool jce_model_importer_inspect_memory(const void *data, size_t size,
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

bool jce_model_importer_inspect_file(const char *file_path,
                                     bool        want_wireframe,
                                     JceModelInspectResult *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!file_path || !file_path[0]) {
        snprintf(out->error, sizeof(out->error), "empty file path");
        return false;
    }
    Assimp::Importer imp;
    const aiScene *scene = imp.ReadFile(file_path, INSPECT_FLAGS);
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