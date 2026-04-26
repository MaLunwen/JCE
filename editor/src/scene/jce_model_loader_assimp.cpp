/*
 * jce_model_loader_assimp.cpp  Model loading via assimp (editor-only).
 *
 * Uses assimp's C++ interface to load models from PAK assets.
 * Supports all assimp-supported formats: OBJ, FBX, 3DS, glTF, etc.
 */

#include "jce_model_loader_assimp.h"
#include "jce_editor_alloc.h"
#include "io/jce_editor_file_util.h"

extern "C" {
#include <jce/os/core/jce_log.h>
}

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>

#include <cstring>
#include <cstdio>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

#define LOG_TAG "editor_model"

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

void jce_editor_model_free_cpu_data(JceEditorCpuMeshData *data)
{
    if (!data) return;
    ED_FREE(data->vertices);
    ED_FREE(data->indices);
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
        ED_CALLOC(total_verts, sizeof(JceMeshVertex)));
    auto *indices = (total_indices > 0)
        ? static_cast<uint32_t *>(ED_MALLOC(
            static_cast<size_t>(total_indices) * sizeof(uint32_t)))
        : nullptr;
    if (!verts || (total_indices > 0 && !indices)) {
        ED_FREE(verts);
        ED_FREE(indices);
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

JceMesh *jce_editor_model_load(const JcePakArchive *pak, const char *asset_path)
{
    if (!pak || !asset_path) return nullptr;

    const JcePakAsset *asset = jce_pak_find(pak, asset_path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "model not found in PAK: %s", asset_path);
        return nullptr;
    }

    void *buf = ED_MALLOC(static_cast<size_t>(asset->original_size));
    if (!buf) return nullptr;

    size_t n = jce_pak_decompress(asset, buf, static_cast<size_t>(asset->original_size));
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", asset_path);
        ED_FREE(buf);
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

    ED_FREE(buf);

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
        if (!ed_write_file(full.c_str(), tex->pcData, tex->mWidth))
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
        uint8_t *blob = (uint8_t *)ED_MALLOC(total);
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

        bool ok = ed_write_file(full.c_str(), blob, total);
        ED_FREE(blob);
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
                                      JceEditorMaterialInfo *out)
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

bool jce_editor_model_extract_material(const char *file_path,
                                       JceEditorMaterialInfo *out)
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

} /* extern "C" */
