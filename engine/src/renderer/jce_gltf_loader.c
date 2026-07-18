/*
 * jce_gltf_loader.c  glTF 2.0 / GLB model loading from PAK.
 *
 * Uses cgltf (single-header C99 glTF parser) to extract meshes,
 * PBR materials, textures, skeleton, and animation clips from a
 * GLB binary blob decompressed from the PAK archive.
 */

#include "jce_gltf_loader.h"

#include <jce/middleware/animation/jce_morph.h>
#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/renderer/jce_skinned_mesh.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <float.h>   /* FLT_MAX (model AABB seed) */
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_texture.h>

#include "middleware/animation/jce_animation.h"
#include <jce/middleware/animation/jce_anim_compress.h>  /* opt-in keyframe reduction */
#include "os/core/jce_memory.h"
#include "renderer/jce_model_internal.h"

#include <cgltf.h>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "jce_gltf"

/* ================================================================== */
/* CPU staging types (worker decode → main-thread upload)              */
/* ================================================================== */

/* In-asset auto-LOD ceiling on the CPU stage (large-world-opt P1 #6).  Matches
 * the renderer-side JCE_SM_MAX_LOD so a parsed level is always uploadable. */
#define JCE_GLTF_MAX_LOD 8

/* One primitive's geometry as CPU arrays (no GPU buffers yet). */
typedef struct {
    int           kind;        /* 0 = static PBR (JcePbrVertex), 1 = skinned */
    void         *verts;       /* owned: JcePbrVertex[] or JceSkinnedVertex[] */
    uint32_t      num_verts;
    uint32_t     *indices;     /* owned, may be NULL */
    uint32_t      num_indices;
    uint32_t      material_index;
    JceMorphData *morph;       /* morph-target deltas (FEATURE 3.1), or NULL */
    /* In-asset auto-LOD index buffers parsed from the JCE_lod primitive
     * extension (one reduced index set per level; all share `verts`).  Empty
     * (lod_count==0) for any mesh cooked without LODs.  Owned; freed in
     * jce_gltf_model_cpu_free; uploaded as alternate IBOs at create time. */
    uint32_t     *lod_indices[JCE_GLTF_MAX_LOD];
    uint32_t      lod_num_indices[JCE_GLTF_MAX_LOD];
    uint32_t      lod_count;
    /* Nanite-lite V1: meshlet sidecar parsed from the JCE_meshlets primitive
     * extension — a meshlet-GROUPED index buffer (each cluster's triangles
     * contiguous, global vertex ids → standard vertex fetch) + per-meshlet
     * {index_offset, index_count} descriptors and {sphere xyzr, cone axis+
     * cutoff} bounds for the GPU cluster cull.  Empty (ml_count==0) for
     * meshes cooked without meshlets.  Owned; freed in model_cpu_free. */
    uint32_t     *ml_indices;      /* meshlet-grouped IB (global vertex ids) */
    uint32_t      ml_num_indices;
    uint32_t     *ml_desc;         /* 2 u32 per meshlet: index_offset, count */
    float        *ml_bounds;       /* 8 f32 per meshlet: cx,cy,cz,r, ax,ay,az,cutoff */
    float        *ml_errors;       /* 10 f32 per meshlet {own,parent, own-group
                                    * sphere xyzr, parent-group sphere xyzr}
                                    * or NULL (V3 cluster-LOD DAG)          */
    uint32_t      ml_count;
} JceModelPrimCpu;

typedef struct {
    char             name[64];
    jce_mat4         local_transform;
    JceModelPrimCpu *prims;
    uint32_t         num_prims;
    int16_t          parent;
    int32_t          joint_parent_index;
    jce_mat4         joint_local_matrix;
} JceModelNodeCpu;

/* Material factors + decoded (not-yet-uploaded) texture maps. */
typedef struct {
    JcePbrMaterial base;       /* factors/alpha/flags; map handles INVALID */
    JceTextureCpu *albedo;
    JceTextureCpu *mr;
    JceTextureCpu *normal;
    JceTextureCpu *ao;
    JceTextureCpu *emissive;
} JceModelMatCpu;

/* One imported morph-weight track: keyframed all-target weights for the model
 * node it drives, tagged with its source animation index (FEATURE 3.1). */
typedef struct {
    uint32_t             anim_index;
    uint32_t             node_index;
    JceMorphWeightTrack *track;       /* CPU-only, owned */
} JceModelMorphAnimCpu;

struct JceModelCpu {
    JceModelNodeCpu      *nodes;      uint32_t num_nodes;
    JceModelMatCpu       *materials;  uint32_t num_materials;
    JceSkeleton          *skeleton;   /* CPU-only, built on the worker */
    JceAnimClip         **anim_clips; uint32_t num_anims;
    JceModelMorphAnimCpu *morph_anims; uint32_t num_morph_anims;
};

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

typedef struct JceGltfPakFileCtx {
    const JcePakArchive *pak;
} JceGltfPakFileCtx;

static cgltf_result jce_gltf_pak_file_read(
    const cgltf_memory_options *memory_options,
    const cgltf_file_options *file_options,
    const char *path,
    cgltf_size *size,
    void **data)
{
    (void)memory_options;
    if (!file_options || !file_options->user_data || !path || !size || !data)
        return cgltf_result_io_error;

    const JceGltfPakFileCtx *ctx =
        (const JceGltfPakFileCtx *)file_options->user_data;
    if (!ctx->pak)
        return cgltf_result_io_error;

    const JcePakAsset *asset = jce_pak_find(ctx->pak, path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "glTF external buffer not found in PAK: %s", path);
        return cgltf_result_file_not_found;
    }
    if (asset->original_size > (uint64_t)SIZE_MAX)
        return cgltf_result_out_of_memory;

    size_t need = (size_t)asset->original_size;
    void *buf = JCE_MALLOC(need);
    if (!buf)
        return cgltf_result_out_of_memory;

    size_t got = jce_pak_decompress_ex(ctx->pak, asset, buf, need);
    if (got != need) {
        JCE_FREE(buf);
        return cgltf_result_io_error;
    }

    *size = (cgltf_size)got;
    *data = buf;
    return cgltf_result_success;
}

static void jce_gltf_pak_file_release(
    const cgltf_memory_options *memory_options,
    const cgltf_file_options *file_options,
    void *data)
{
    (void)memory_options;
    (void)file_options;
    JCE_FREE(data);
}

/* Resolve a texture URI relative to the model's directory in PAK. */
static void resolve_path(const char *model_path, const char *uri,
                          char *out, size_t out_size)
{
    /* Find last '/' in model_path to extract the directory. */
    const char *slash = strrchr(model_path, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - model_path + 1);
        if (dir_len >= out_size) dir_len = out_size - 1;
        memcpy(out, model_path, dir_len);
        out[dir_len] = '\0';

        size_t uri_len = strlen(uri);
        if (dir_len + uri_len < out_size) {
            memcpy(out + dir_len, uri, uri_len + 1);
        } else {
            out[0] = '\0';
        }
    } else {
        /* No directory component; use URI as-is. */
        size_t len = strlen(uri);
        if (len < out_size) {
            memcpy(out, uri, len + 1);
        } else {
            out[0] = '\0';
        }
    }
}

/* Decode a glTF image (URI or embedded buffer_view) to a CPU texture
 * result — NO bgfx, so it is safe to run on a worker thread.  The caller
 * uploads it later with jce_texture_upload_cpu (or frees it on cancel).
 * Returns NULL when the image cannot be decoded. */
static JceTextureCpu *load_gltf_texture_cpu(const JcePakArchive *pak,
                                            const char *model_path,
                                            const cgltf_image *image,
                                            cgltf_data *data)
{
    if (!image) return NULL;

    /* Case 1: embedded texture via buffer_view (common in GLB). */
    if (image->buffer_view) {
        const cgltf_buffer_view *bv = image->buffer_view;
        if (bv->buffer && bv->buffer->data) {
            const uint8_t *img_data = (const uint8_t *)bv->buffer->data
                                      + bv->offset;
            JceTextureCpu *c = jce_texture_decode_cpu_mem(
                img_data, (size_t)bv->size, JCE_TEX_WRAP);
            if (!c)
                LOG_ERROR(LOG_TAG, "failed to decode embedded texture");
            return c;
        }
    }

    /* Case 2: external URI -- resolve relative to model path. Try PAK first
     * (runtime), then fall back to disk (editor / dev workflow). */
    if (image->uri) {
        char resolved[512];
        resolve_path(model_path, image->uri, resolved, sizeof(resolved));
        JceTextureCpu *c = jce_texture_decode_cpu(pak, resolved, JCE_TEX_WRAP);
        if (c) return c;

        /* Disk fallback: build absolute path from model_path's directory. */
        char disk_path[1024];
        const char *slash = strrchr(model_path, '/');
        const char *bslash = strrchr(model_path, '\\');
        const char *sep = (slash > bslash) ? slash : bslash;
        if (sep) {
            size_t dir_len = (size_t)(sep - model_path + 1);
            if (dir_len < sizeof(disk_path)) {
                memcpy(disk_path, model_path, dir_len);
                SDL_strlcpy(disk_path + dir_len, image->uri,
                            sizeof(disk_path) - dir_len);
            } else {
                SDL_strlcpy(disk_path, image->uri, sizeof(disk_path));
            }
        } else {
            SDL_strlcpy(disk_path, image->uri, sizeof(disk_path));
        }

        /* Disk fallback only when there is no PAK to serve from (i.e. editor
         * / dev path).  In a deployed game the PAK overlay chain must contain
         * every texture; reaching out to host disk would break portability. */
        if (!pak) {
            uint64_t img_size = 0;
            void *img_buf = jce_fs_host_read_all(disk_path, &img_size);
            if (img_buf && img_size > 0) {
                JceTextureCpu *cc = jce_texture_decode_cpu_mem(
                    img_buf, (size_t)img_size, JCE_TEX_WRAP);
                JCE_FREE(img_buf);
                return cc;
            }
            if (img_buf) JCE_FREE(img_buf);
        } else {
            LOG_WARN("gltf", "texture missing from pak (no host fallback in runtime): %s",
                     resolved);
        }
        return NULL;
    }

    (void)data;
    return NULL;
}

/* Find joint index in a skin's joints array for a given node. */
static int find_joint_index(const cgltf_skin *skin, const cgltf_node *node)
{
    if (!skin || !node) return -1;
    cgltf_size i;
    for (i = 0; i < skin->joints_count; ++i) {
        if (skin->joints[i] == node)
            return (int)i;
    }
    return -1;
}

/* Find the material index within cgltf_data for a given material pointer. */
static uint32_t find_material_index(const cgltf_data *data,
                                     const cgltf_material *mat)
{
    if (!mat || !data->materials) return 0;
    cgltf_size idx = (cgltf_size)(mat - data->materials);
    return (idx < data->materials_count) ? (uint32_t)idx : 0;
}

/* ================================================================== */
/* Extract PBR materials                                               */
/* ================================================================== */

/* Extract materials to CPU staging: factors + flags into `base` (with
 * INVALID map handles), and each texture decoded to a JceTextureCpu for
 * later GPU upload.  No bgfx — safe on a worker thread. */
static JceModelMatCpu *extract_materials_cpu(const JcePakArchive *pak,
                                             const char *model_path,
                                             cgltf_data *data,
                                             uint32_t *out_count)
{
    *out_count = 0;
    if (data->materials_count == 0) {
        /* Create a single default material. */
        JceModelMatCpu *mats = (JceModelMatCpu *)JCE_CALLOC(1, sizeof(JceModelMatCpu));
        if (!mats) return NULL;
        mats[0].base = jce_pbr_material_default();
        *out_count = 1;
        return mats;
    }

    uint32_t count = (uint32_t)data->materials_count;
    JceModelMatCpu *mats = (JceModelMatCpu *)JCE_CALLOC(count, sizeof(JceModelMatCpu));
    if (!mats) return NULL;

    uint32_t i;
    for (i = 0; i < count; ++i) {
        const cgltf_material *src = &data->materials[i];
        JceModelMatCpu *m = &mats[i];
        JcePbrMaterial *dst = &m->base;

        /* Start with defaults (map handles stay INVALID). */
        *dst = jce_pbr_material_default();

        /* PBR metallic-roughness factors. */
        if (src->has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness *pbr = &src->pbr_metallic_roughness;

            dst->base_color_factor[0] = pbr->base_color_factor[0];
            dst->base_color_factor[1] = pbr->base_color_factor[1];
            dst->base_color_factor[2] = pbr->base_color_factor[2];
            dst->base_color_factor[3] = pbr->base_color_factor[3];
            dst->metallic_factor  = pbr->metallic_factor;
            dst->roughness_factor = pbr->roughness_factor;

            if (pbr->base_color_texture.texture &&
                pbr->base_color_texture.texture->image) {
                m->albedo = load_gltf_texture_cpu(
                    pak, model_path, pbr->base_color_texture.texture->image, data);
            }
            if (pbr->metallic_roughness_texture.texture &&
                pbr->metallic_roughness_texture.texture->image) {
                m->mr = load_gltf_texture_cpu(
                    pak, model_path,
                    pbr->metallic_roughness_texture.texture->image, data);
            }
        }

        /* Normal map. */
        if (src->normal_texture.texture &&
            src->normal_texture.texture->image) {
            m->normal = load_gltf_texture_cpu(
                pak, model_path, src->normal_texture.texture->image, data);
            dst->normal_scale = src->normal_texture.scale;
            if (dst->normal_scale == 0.0f) dst->normal_scale = 1.0f;
        }

        /* AO map. */
        if (src->occlusion_texture.texture &&
            src->occlusion_texture.texture->image) {
            m->ao = load_gltf_texture_cpu(
                pak, model_path, src->occlusion_texture.texture->image, data);
            dst->ao_strength = src->occlusion_texture.scale;
            if (dst->ao_strength == 0.0f) dst->ao_strength = 1.0f;
        }

        /* Emissive. */
        if (src->emissive_texture.texture &&
            src->emissive_texture.texture->image) {
            m->emissive = load_gltf_texture_cpu(
                pak, model_path, src->emissive_texture.texture->image, data);
        }
        dst->emissive_factor[0] = src->emissive_factor[0];
        dst->emissive_factor[1] = src->emissive_factor[1];
        dst->emissive_factor[2] = src->emissive_factor[2];

        /* Alpha mode. */
        switch (src->alpha_mode) {
        case cgltf_alpha_mode_mask:   dst->alpha_mode = JCE_ALPHA_MASK;   break;
        case cgltf_alpha_mode_blend:  dst->alpha_mode = JCE_ALPHA_BLEND;  break;
        default:                      dst->alpha_mode = JCE_ALPHA_OPAQUE; break;
        }
        dst->alpha_cutoff  = src->alpha_cutoff;
        dst->double_sided  = src->double_sided ? true : false;
    }

    *out_count = count;
    return mats;
}

/* ================================================================== */
/* Extract mesh primitives                                             */
/* ================================================================== */

/* ── In-asset auto-LOD: parse the JCE_lod primitive extension (P1 #6) ──────
 * The cook emits, on a primitive's "extensions", a JCE_lod object whose raw
 * JSON cgltf stores verbatim in cgltf_extension.data, e.g.
 *     {"indices":[7,9,11]}
 * where each value is a glTF accessor index for a reduced SCALAR uint32 index
 * buffer sharing the primitive's POSITION vertex buffer.  cgltf parses ALL
 * accessors into data->accessors[] regardless of references, so we resolve each
 * listed index there.  A tiny hand-parser (no JSON dep in the renderer layer)
 * extracts the integers from the "indices":[ ... ] array of this small,
 * cook-controlled object.  Reads each accessor into a fresh uint32_t buffer on
 * `out`; sets out->lod_count.  No-op when the primitive has no JCE_lod. */
static void build_primitive_lods(const cgltf_primitive *prim,
                                 const cgltf_data *data,
                                 uint32_t num_verts,
                                 JceModelPrimCpu *out)
{
    if (!prim || !data || !out) return;
    const char *json = NULL;
    for (cgltf_size i = 0; i < prim->extensions_count; ++i) {
        if (prim->extensions[i].name &&
            strcmp(prim->extensions[i].name, "JCE_lod") == 0) {
            json = prim->extensions[i].data;
            break;
        }
    }
    if (!json) return;

    const char *arr = strstr(json, "\"indices\"");
    if (!arr) return;
    arr = strchr(arr, '[');
    if (!arr) return;
    ++arr;

    /* Walk the array, reading each accessor index, resolving it against
     * data->accessors[], and copying the indices into a CPU buffer. */
    while (*arr && *arr != ']' && out->lod_count < JCE_GLTF_MAX_LOD) {
        while (*arr == ' ' || *arr == ',' || *arr == '\t' ||
               *arr == '\n' || *arr == '\r') ++arr;
        if (*arr < '0' || *arr > '9') break;
        char *end = NULL;
        long acc = strtol(arr, &end, 10);
        if (end == arr) break;
        arr = end;
        if (acc < 0 || (cgltf_size)acc >= data->accessors_count) continue;

        const cgltf_accessor *a = &data->accessors[acc];
        uint32_t nidx = (uint32_t)a->count;
        if (nidx < 3 || (nidx % 3) != 0) continue;
        uint32_t *idx = (uint32_t *)JCE_MALLOC((size_t)nidx * sizeof(uint32_t));
        if (!idx) continue;
        bool ok = true;
        for (uint32_t ii = 0; ii < nidx; ++ii) {
            uint32_t v = (uint32_t)cgltf_accessor_read_index(a, ii);
            if (v >= num_verts) { ok = false; break; }   /* must address base VB */
            idx[ii] = v;
        }
        if (!ok) { JCE_FREE(idx); continue; }
        out->lod_indices[out->lod_count]     = idx;
        out->lod_num_indices[out->lod_count] = nidx;
        out->lod_count++;
    }
}

/* ── Nanite-lite V1: parse the JCE_meshlets primitive extension ────────────
 * The cook (or a hand-authored asset) emits on a primitive's "extensions":
 *     {"JCE_meshlets":{"indices":A,"meshlets":B,"bounds":C}}
 * where A = SCALAR u32 accessor holding the meshlet-GROUPED index buffer
 * (every cluster's triangles contiguous, GLOBAL vertex ids so the standard
 * vertex pipeline consumes it), B = SCALAR u32 accessor with 2 values per
 * meshlet {index_offset, index_count}, C = SCALAR f32 accessor with 8 values
 * per meshlet {sphere cx,cy,cz,r, cone ax,ay,az,cutoff} (meshopt cone
 * convention: cullable when dot(center-cam, axis) >= cutoff*|center-cam|+r).
 * Same tiny hand-parser as JCE_lod (cook-controlled object; no JSON dep). */
static bool ml_read_key_int(const char *json, const char *key, long *out)
{
    const char *p = strstr(json, key);
    if (!p) return false;
    p = strchr(p + strlen(key), ':');
    if (!p) return false;
    char *end = NULL;
    long v = strtol(p + 1, &end, 10);
    if (end == p + 1) return false;
    *out = v;
    return true;
}

static void build_primitive_meshlets(const cgltf_primitive *prim,
                                     const cgltf_data *data,
                                     uint32_t num_verts,
                                     JceModelPrimCpu *out)
{
    if (!prim || !data || !out) return;
    const char *json = NULL;
    for (cgltf_size i = 0; i < prim->extensions_count; ++i) {
        if (prim->extensions[i].name &&
            strcmp(prim->extensions[i].name, "JCE_meshlets") == 0) {
            json = prim->extensions[i].data;
            break;
        }
    }
    if (!json) return;

    long ai = -1, bi = -1, ci = -1, ei = -1;
    if (!ml_read_key_int(json, "\"indices\"", &ai) ||
        !ml_read_key_int(json, "\"meshlets\"", &bi) ||
        !ml_read_key_int(json, "\"bounds\"", &ci))
        return;
    if (ai < 0 || (cgltf_size)ai >= data->accessors_count ||
        bi < 0 || (cgltf_size)bi >= data->accessors_count ||
        ci < 0 || (cgltf_size)ci >= data->accessors_count)
        return;
    /* Optional V3 cluster-LOD-DAG errors accessor ({own,parent} pairs). */
    if (!ml_read_key_int(json, "\"errors\"", &ei) ||
        ei < 0 || (cgltf_size)ei >= data->accessors_count)
        ei = -1;

    const cgltf_accessor *A = &data->accessors[ai];
    const cgltf_accessor *B = &data->accessors[bi];
    const cgltf_accessor *C = &data->accessors[ci];
    const cgltf_accessor *E = (ei >= 0) ? &data->accessors[ei] : NULL;
    uint32_t nidx = (uint32_t)A->count;
    uint32_t mcnt = (uint32_t)(B->count / 2u);
    if (nidx < 3u || mcnt == 0u || (uint32_t)C->count != mcnt * 8u) return;
    if (E && (uint32_t)E->count != mcnt * 10u) E = NULL;

    uint32_t *idx    = (uint32_t *)JCE_MALLOC((size_t)nidx * sizeof(uint32_t));
    uint32_t *desc   = (uint32_t *)JCE_MALLOC((size_t)mcnt * 2u * sizeof(uint32_t));
    float    *bounds = (float *)JCE_MALLOC((size_t)mcnt * 8u * sizeof(float));
    float    *errs   = E ? (float *)JCE_MALLOC((size_t)mcnt * 10u * sizeof(float))
                         : NULL;
    if (!idx || !desc || !bounds || (E && !errs)) goto fail;

    for (uint32_t i = 0; i < nidx; ++i) {
        uint32_t v = (uint32_t)cgltf_accessor_read_index(A, i);
        if (v >= num_verts) goto fail;   /* must address the base VB */
        idx[i] = v;
    }
    for (uint32_t i = 0; i < mcnt * 2u; ++i)
        desc[i] = (uint32_t)cgltf_accessor_read_index(B, i);
    for (uint32_t i = 0; i < mcnt * 8u; ++i) {
        cgltf_float f = 0.0f;
        cgltf_accessor_read_float(C, i, &f, 1);
        bounds[i] = (float)f;
    }
    if (errs) {
        for (uint32_t i = 0; i < mcnt * 10u; ++i) {
            cgltf_float f = 0.0f;
            cgltf_accessor_read_float(E, i, &f, 1);
            errs[i] = (float)f;
        }
    }
    /* Descriptor sanity: every cluster range inside the meshlet IB. */
    for (uint32_t m = 0; m < mcnt; ++m)
        if ((uint64_t)desc[m * 2u] + desc[m * 2u + 1u] > nidx) goto fail;

    out->ml_indices     = idx;
    out->ml_num_indices = nidx;
    out->ml_desc        = desc;
    out->ml_bounds      = bounds;
    out->ml_errors      = errs;
    out->ml_count       = mcnt;
    return;
fail:
    if (idx)    JCE_FREE(idx);
    if (desc)   JCE_FREE(desc);
    if (bounds) JCE_FREE(bounds);
    if (errs)   JCE_FREE(errs);
}

/* Find an attribute accessor by type within a primitive. */
static const cgltf_accessor *find_attribute(const cgltf_primitive *prim,
                                             cgltf_attribute_type type,
                                             int index)
{
    cgltf_size i;
    for (i = 0; i < prim->attributes_count; ++i) {
        if (prim->attributes[i].type == type &&
            prim->attributes[i].index == index)
            return prim->attributes[i].data;
    }
    return NULL;
}

/* Find a morph-target attribute accessor by type (POSITION / NORMAL). */
static const cgltf_accessor *find_morph_attribute(const cgltf_morph_target *tgt,
                                                  cgltf_attribute_type type)
{
    cgltf_size i;
    for (i = 0; i < tgt->attributes_count; ++i) {
        if (tgt->attributes[i].type == type)
            return tgt->attributes[i].data;
    }
    return NULL;
}

/* Build morph storage for a primitive's targets (POSITION + optional NORMAL
 * deltas) and seed the base weights from the mesh-/node-level defaults.
 * Returns NULL when the primitive has no targets or no usable POSITION delta.
 * The base weights array (if present) lives on the cgltf_mesh that owns the
 * primitive — passed in via `mesh_weights`. */
static JceMorphData *build_primitive_morph(const cgltf_primitive *prim,
                                           uint32_t num_verts,
                                           const cgltf_float *mesh_weights,
                                           cgltf_size mesh_weights_count)
{
    uint32_t num_targets = (uint32_t)prim->targets_count;
    if (num_targets == 0 || num_verts == 0) return NULL;

    /* Detect whether ANY target carries NORMAL deltas; allocate the parallel
     * normal-delta array only when at least one does. */
    bool has_normals = false;
    for (uint32_t t = 0; t < num_targets; ++t) {
        if (find_morph_attribute(&prim->targets[t], cgltf_attribute_type_normal))
            { has_normals = true; break; }
    }

    JceMorphData *morph = jce_morph_data_create(num_targets, num_verts,
                                                has_normals);
    if (!morph) return NULL;

    bool any_position = false;
    for (uint32_t t = 0; t < num_targets; ++t) {
        const cgltf_morph_target *tgt = &prim->targets[t];

        const cgltf_accessor *a_dpos =
            find_morph_attribute(tgt, cgltf_attribute_type_position);
        if (a_dpos && a_dpos->count >= num_verts) {
            jce_vec3 *deltas = (jce_vec3 *)JCE_MALLOC(
                (size_t)num_verts * sizeof(jce_vec3));
            if (deltas) {
                for (uint32_t v = 0; v < num_verts; ++v) {
                    float d[3] = { 0.0f, 0.0f, 0.0f };
                    cgltf_accessor_read_float(a_dpos, v, d, 3);
                    deltas[v].x = d[0]; deltas[v].y = d[1]; deltas[v].z = d[2];
                }
                jce_morph_set_position_deltas(morph, t, deltas, num_verts);
                JCE_FREE(deltas);
                any_position = true;
            }
        }

        if (has_normals) {
            const cgltf_accessor *a_dnorm =
                find_morph_attribute(tgt, cgltf_attribute_type_normal);
            if (a_dnorm && a_dnorm->count >= num_verts) {
                jce_vec3 *deltas = (jce_vec3 *)JCE_MALLOC(
                    (size_t)num_verts * sizeof(jce_vec3));
                if (deltas) {
                    for (uint32_t v = 0; v < num_verts; ++v) {
                        float d[3] = { 0.0f, 0.0f, 0.0f };
                        cgltf_accessor_read_float(a_dnorm, v, d, 3);
                        deltas[v].x = d[0]; deltas[v].y = d[1]; deltas[v].z = d[2];
                    }
                    jce_morph_set_normal_deltas(morph, t, deltas, num_verts);
                    JCE_FREE(deltas);
                }
            }
        }

        /* Base weight: glTF stores defaults per mesh (or node); index by target. */
        if (mesh_weights && (cgltf_size)t < mesh_weights_count)
            jce_morph_set_base_weight(morph, t, (float)mesh_weights[t]);
    }

    if (!any_position) {
        /* No target produced a POSITION delta — nothing to morph. */
        jce_morph_data_destroy(morph);
        return NULL;
    }

    return morph;
}

/* Extract one glTF primitive's geometry into CPU staging arrays (no GPU
 * buffers; jce_gltf_upload_cpu creates them later).  Ownership of verts +
 * indices transfers to `out`. */
static void build_primitive_cpu(const cgltf_primitive *prim,
                                JceModelPrimCpu *out,
                                const cgltf_data *data,
                                const cgltf_float *base_weights,
                                cgltf_size base_weights_count)
{
    const cgltf_accessor *a_pos    = find_attribute(prim, cgltf_attribute_type_position, 0);
    const cgltf_accessor *a_norm   = find_attribute(prim, cgltf_attribute_type_normal, 0);
    const cgltf_accessor *a_uv     = find_attribute(prim, cgltf_attribute_type_texcoord, 0);
    const cgltf_accessor *a_tan    = find_attribute(prim, cgltf_attribute_type_tangent, 0);
    const cgltf_accessor *a_joints = find_attribute(prim, cgltf_attribute_type_joints, 0);
    const cgltf_accessor *a_wts    = find_attribute(prim, cgltf_attribute_type_weights, 0);

    if (!a_pos || a_pos->count == 0) return;

    uint32_t num_verts = (uint32_t)a_pos->count;

    /* Read indices. */
    uint32_t num_indices = 0;
    uint32_t *indices = NULL;
    if (prim->indices) {
        num_indices = (uint32_t)prim->indices->count;
        indices = (uint32_t *)JCE_MALLOC(num_indices * sizeof(uint32_t));
        if (indices) {
            uint32_t ii;
            for (ii = 0; ii < num_indices; ++ii) {
                indices[ii] = (uint32_t)cgltf_accessor_read_index(prim->indices, ii);
            }
        }
    }

    /* Material index. */
    out->material_index = find_material_index(data, prim->material);

    /* In-asset auto-LOD (P1 #6): parse the JCE_lod extension's reduced index
     * accessors.  No-op for meshes cooked without LODs. */
    build_primitive_lods(prim, data, num_verts, out);
    build_primitive_meshlets(prim, data, num_verts, out);   /* Nanite-lite V1 */

    /* Morph targets / blendshapes (FEATURE 3.1): read POSITION (+NORMAL) deltas
     * per target and seed the base weights.  No-op when the primitive has no
     * targets. */
    out->morph = build_primitive_morph(prim, num_verts,
                                       base_weights, base_weights_count);

    /* ---- Skinned mesh ---- */
    if (a_joints && a_wts) {
        JceSkinnedVertex *verts = (JceSkinnedVertex *)JCE_CALLOC(
            num_verts, sizeof(JceSkinnedVertex));
        if (!verts) { JCE_FREE(indices); return; }

        uint32_t vi;
        for (vi = 0; vi < num_verts; ++vi) {
            float tmp[4];

            cgltf_accessor_read_float(a_pos, vi, verts[vi].pos, 3);

            if (a_norm) cgltf_accessor_read_float(a_norm, vi, verts[vi].normal, 3);
            else { verts[vi].normal[0] = 0; verts[vi].normal[1] = 1; verts[vi].normal[2] = 0; }

            if (a_uv) cgltf_accessor_read_float(a_uv, vi, verts[vi].uv, 2);

            if (a_tan) {
                cgltf_accessor_read_float(a_tan, vi, verts[vi].tangent, 4);
            } else {
                verts[vi].tangent[0] = 1; verts[vi].tangent[3] = 1;
            }

            /* Joint indices. */
            cgltf_uint j4[4] = {0};
            cgltf_accessor_read_uint(a_joints, vi, j4, 4);
            verts[vi].joints[0] = (uint8_t)j4[0];
            verts[vi].joints[1] = (uint8_t)j4[1];
            verts[vi].joints[2] = (uint8_t)j4[2];
            verts[vi].joints[3] = (uint8_t)j4[3];

            /* Bone weights. */
            cgltf_accessor_read_float(a_wts, vi, tmp, 4);
            verts[vi].weights[0] = tmp[0];
            verts[vi].weights[1] = tmp[1];
            verts[vi].weights[2] = tmp[2];
            verts[vi].weights[3] = tmp[3];
        }

        out->kind        = 1;   /* skinned */
        out->verts       = verts;
        out->num_verts   = num_verts;
        out->indices     = indices;
        out->num_indices = num_indices;
        return;   /* ownership of verts+indices transferred to `out` */
    }

    /* ---- Static PBR mesh (with tangent) ---- */
    JcePbrVertex *verts = (JcePbrVertex *)JCE_CALLOC(num_verts, sizeof(JcePbrVertex));
    if (!verts) { JCE_FREE(indices); return; }

    uint32_t vi;
    for (vi = 0; vi < num_verts; ++vi) {
        cgltf_accessor_read_float(a_pos, vi, verts[vi].pos, 3);

        if (a_norm) cgltf_accessor_read_float(a_norm, vi, verts[vi].normal, 3);
        else { verts[vi].normal[0] = 0; verts[vi].normal[1] = 1; verts[vi].normal[2] = 0; }

        if (a_uv) cgltf_accessor_read_float(a_uv, vi, verts[vi].uv, 2);

        if (a_tan) {
            cgltf_accessor_read_float(a_tan, vi, verts[vi].tangent, 4);
        } else {
            /* No tangent in source → default +X, handedness +1. */
            verts[vi].tangent[0] = 1.0f;
            verts[vi].tangent[1] = 0.0f;
            verts[vi].tangent[2] = 0.0f;
            verts[vi].tangent[3] = 1.0f;
        }
    }

    out->kind        = 0;   /* static PBR */
    out->verts       = verts;
    out->num_verts   = num_verts;
    out->indices     = indices;
    out->num_indices = num_indices;
}

/* ================================================================== */
/* Extract skeleton                                                    */
/* ================================================================== */

static JceSkeleton *extract_skeleton(cgltf_data *data)
{
    if (data->skins_count == 0) return NULL;

    const cgltf_skin *skin = &data->skins[0];
    uint32_t num_joints = (uint32_t)skin->joints_count;
    if (num_joints == 0) return NULL;

    JceJoint *joints = (JceJoint *)JCE_CALLOC(num_joints, sizeof(JceJoint));
    if (!joints) return NULL;

    /* Node above the root joints (the armature). Carries any model-level
     * orientation/scale that must be applied to the whole skeleton. */
    const cgltf_node *armature_node = NULL;

    uint32_t ji;
    for (ji = 0; ji < num_joints; ++ji) {
        const cgltf_node *jnode = skin->joints[ji];
        JceJoint *j = &joints[ji];

        /* Name. */
        if (jnode->name) {
            size_t len = strlen(jnode->name);
            if (len >= sizeof(j->name)) len = sizeof(j->name) - 1;
            memcpy(j->name, jnode->name, len);
            j->name[len] = '\0';
        } else {
            SDL_snprintf(j->name, sizeof(j->name), "joint_%u", ji);
        }

        /* Parent index: find if the node's parent is also a joint. */
        j->parent = -1;
        if (jnode->parent) {
            int pidx = find_joint_index(skin, jnode->parent);
            if (pidx >= 0) j->parent = (int16_t)pidx;
            else if (!armature_node) armature_node = jnode->parent;
        }

        /* Inverse bind matrix. */
        if (skin->inverse_bind_matrices &&
            ji < (uint32_t)skin->inverse_bind_matrices->count) {
            cgltf_accessor_read_float(skin->inverse_bind_matrices, ji,
                                       j->inverse_bind_matrix.raw[0], 16);
        } else {
            j->inverse_bind_matrix = jce_m4_identity();
        }

        /* Local rest-pose transform. */
        cgltf_node_transform_local(jnode, j->local_transform.raw[0]);

        /* Rest-pose TRS from glTF node (avoids decomposition roundtrip). */
        if (jnode->has_translation) {
            j->rest_translation = jce_v3(jnode->translation[0],
                                          jnode->translation[1],
                                          jnode->translation[2]);
        } else {
            j->rest_translation = jce_v3(0.0f, 0.0f, 0.0f);
        }
        if (jnode->has_rotation) {
            j->rest_rotation.x = jnode->rotation[0];
            j->rest_rotation.y = jnode->rotation[1];
            j->rest_rotation.z = jnode->rotation[2];
            j->rest_rotation.w = jnode->rotation[3];
        } else {
            j->rest_rotation = jce_q_identity();
        }
        if (jnode->has_scale) {
            j->rest_scale = jce_v3(jnode->scale[0],
                                    jnode->scale[1],
                                    jnode->scale[2]);
        } else {
            j->rest_scale = jce_v3(1.0f, 1.0f, 1.0f);
        }
    }

    JceSkeleton *skel = jce_skeleton_create(joints, num_joints);
    JCE_FREE(joints);

    /* Bake the armature's world transform so root joints inherit any model-level
     * orientation/scale (e.g. CesiumMan's Z-up->Y-up rotation), mirroring what
     * aiProcess_PreTransformVertices does for the static mesh path. */
    if (skel && armature_node) {
        jce_mat4 rootx;
        cgltf_node_transform_world(armature_node, rootx.raw[0]);
        jce_skeleton_set_root_transform(skel, &rootx);
    }

    return skel;
}

/* ================================================================== */
/* Extract animations                                                  */
/* ================================================================== */

static JceAnimClip **extract_animations(cgltf_data *data, uint32_t *out_count)
{
    *out_count = 0;
    if (data->animations_count == 0 || data->skins_count == 0)
        return NULL;

    const cgltf_skin *skin = &data->skins[0];
    uint32_t num_anims = (uint32_t)data->animations_count;

    JceAnimClip **clips = (JceAnimClip **)JCE_CALLOC(num_anims, sizeof(JceAnimClip *));
    if (!clips) return NULL;

    uint32_t ai;
    for (ai = 0; ai < num_anims; ++ai) {
        const cgltf_animation *anim = &data->animations[ai];
        uint32_t num_channels = (uint32_t)anim->channels_count;

        /* Allocate temporary channel descriptors. */
        JceAnimChannel *channels = (JceAnimChannel *)JCE_CALLOC(
            num_channels, sizeof(JceAnimChannel));
        if (!channels) continue;

        float duration = 0.0f;
        uint32_t valid_channels = 0;

        uint32_t ci;
        for (ci = 0; ci < num_channels; ++ci) {
            const cgltf_animation_channel *ch = &anim->channels[ci];
            if (!ch->sampler || !ch->target_node) continue;

            const cgltf_animation_sampler *samp = ch->sampler;
            if (!samp->input || !samp->output) continue;

            /* Find joint index. */
            int jidx = find_joint_index(skin, ch->target_node);
            if (jidx < 0) continue;

            JceAnimChannel *dst = &channels[valid_channels];
            dst->joint_index = (uint32_t)jidx;

            /* Target path. */
            switch (ch->target_path) {
            case cgltf_animation_path_type_translation:
                dst->target = JCE_ANIM_TARGET_TRANSLATION;
                break;
            case cgltf_animation_path_type_rotation:
                dst->target = JCE_ANIM_TARGET_ROTATION;
                break;
            case cgltf_animation_path_type_scale:
                dst->target = JCE_ANIM_TARGET_SCALE;
                break;
            default:
                /* The skeletal JceAnimClip only carries joint TRS; the "weights"
                 * morph channel is no longer dropped — extract_morph_anims()
                 * collects it into a JceMorphWeightTrack (FEATURE 3.1). */
                continue;
            }

            /* Interpolation.  cgltf packs cubic-spline outputs as
             * [in_tangent, value, out_tangent] per key (3x the input count);
             * the value loops below extract only the value sub-element and the
             * clip is treated as LINEAR (the sampler does not Hermite-eval).
             * Reading the raw index stored the in-tangent as the value and
             * silently corrupted the animation (audit F46). */
            const int cubic =
                (samp->interpolation == cgltf_interpolation_type_cubic_spline);
            switch (samp->interpolation) {
            case cgltf_interpolation_type_step:
                dst->interpolation = JCE_INTERP_STEP;
                break;
            case cgltf_interpolation_type_cubic_spline:
                dst->interpolation = JCE_INTERP_LINEAR;
                break;
            default:
                dst->interpolation = JCE_INTERP_LINEAR;
                break;
            }

            /* Timestamps. */
            uint32_t kf_count = (uint32_t)samp->input->count;
            dst->count = kf_count;

            dst->timestamps = (float *)JCE_MALLOC(kf_count * sizeof(float));
            if (!dst->timestamps) continue;

            uint32_t ki;
            for (ki = 0; ki < kf_count; ++ki) {
                cgltf_accessor_read_float(samp->input, ki,
                                           &dst->timestamps[ki], 1);
                if (dst->timestamps[ki] > duration)
                    duration = dst->timestamps[ki];
            }

            /* Values. */
            if (dst->target == JCE_ANIM_TARGET_TRANSLATION ||
                dst->target == JCE_ANIM_TARGET_SCALE) {
                jce_vec3 *vals = (jce_vec3 *)JCE_MALLOC(kf_count * sizeof(jce_vec3));
                if (!vals) { JCE_FREE(dst->timestamps); dst->timestamps = NULL; continue; }
                for (ki = 0; ki < kf_count; ++ki) {
                    float v[3];
                    cgltf_accessor_read_float(samp->output,
                                              cubic ? ki * 3u + 1u : ki, v, 3);
                    vals[ki].x = v[0];
                    vals[ki].y = v[1];
                    vals[ki].z = v[2];
                }
                if (dst->target == JCE_ANIM_TARGET_TRANSLATION)
                    dst->translations = vals;
                else
                    dst->scales = vals;
            } else {
                /* Rotation (quaternion xyzw). */
                jce_quat *vals = (jce_quat *)JCE_MALLOC(kf_count * sizeof(jce_quat));
                if (!vals) { JCE_FREE(dst->timestamps); dst->timestamps = NULL; continue; }
                for (ki = 0; ki < kf_count; ++ki) {
                    float v[4];
                    cgltf_accessor_read_float(samp->output,
                                              cubic ? ki * 3u + 1u : ki, v, 4);
                    vals[ki].x = v[0];
                    vals[ki].y = v[1];
                    vals[ki].z = v[2];
                    vals[ki].w = v[3];
                }
                dst->rotations = vals;
            }

            valid_channels++;
        }

        /* Optional lossy keyframe reduction (opt-in import setting; default OFF
         * so the load path is byte-identical).  Only LINEAR tracks: the reducer
         * reconstructs via lerp/nlerp, so STEP tracks must keep every key. */
        if (jce_anim_compress_is_enabled()) {
            JceAnimCompressParams cp;
            jce_anim_compress_get_params(&cp);
            for (ci = 0; ci < valid_channels; ++ci) {
                JceAnimChannel *c = &channels[ci];
                if (c->interpolation != JCE_INTERP_LINEAR || !c->timestamps) continue;
                if (c->target == JCE_ANIM_TARGET_ROTATION) {
                    if (c->rotations)
                        c->count = jce_anim_compress_track(c->timestamps, c->rotations,
                            c->count, JCE_ANIM_COMPRESS_QUAT, &cp);
                } else {
                    jce_vec3 *vals = (c->target == JCE_ANIM_TARGET_TRANSLATION)
                                         ? c->translations : c->scales;
                    if (vals)
                        c->count = jce_anim_compress_track(c->timestamps, vals,
                            c->count, JCE_ANIM_COMPRESS_VEC3, &cp);
                }
            }
        }

        /* Create clip name. */
        const char *name = anim->name ? anim->name : "unnamed";

        clips[ai] = jce_anim_clip_create(name, channels, valid_channels, duration);

        /* Free temporary channel data. */
        for (ci = 0; ci < valid_channels; ++ci) {
            JCE_FREE(channels[ci].timestamps);
            JCE_FREE(channels[ci].translations);
            JCE_FREE(channels[ci].rotations);
            JCE_FREE(channels[ci].scales);
        }
        JCE_FREE(channels);
    }

    *out_count = num_anims;
    return clips;
}

/* ================================================================== */
/* Extract morph-weight animation channels (FEATURE 3.1)               */
/* ================================================================== */

/* Map a cgltf_node to its index in the model's filtered node array (only
 * mesh-bearing nodes are kept, in document order).  Returns -1 if `node`
 * has no mesh (so was not staged). */
static int32_t model_node_index_of(const cgltf_data *data,
                                    const cgltf_node *node)
{
    if (!node || !node->mesh) return -1;
    int32_t idx = 0;
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        if (!data->nodes[ni].mesh) continue;
        if (&data->nodes[ni] == node) return idx;
        idx++;
    }
    return -1;
}

/* Un-drop the glTF "weights" animation channel (previously skipped in
 * extract_animations): each weights channel drives ALL morph targets of its
 * target node's mesh over time.  cgltf packs the sampler output as
 * (num_keys * num_targets) scalars, key-major.  Collect one JceMorphWeightTrack
 * per (animation, mesh node) pair.  Independent of skins so static blendshape
 * meshes work too. */
static JceModelMorphAnimCpu *extract_morph_anims(cgltf_data *data,
                                                 uint32_t *out_count)
{
    *out_count = 0;
    if (data->animations_count == 0) return NULL;

    /* Upper bound: every channel could be a distinct weights channel. */
    cgltf_size cap = 0;
    for (cgltf_size ai = 0; ai < data->animations_count; ++ai)
        cap += data->animations[ai].channels_count;
    if (cap == 0) return NULL;

    JceModelMorphAnimCpu *out = (JceModelMorphAnimCpu *)JCE_CALLOC(
        (size_t)cap, sizeof(JceModelMorphAnimCpu));
    if (!out) return NULL;

    uint32_t n = 0;
    for (cgltf_size ai = 0; ai < data->animations_count; ++ai) {
        const cgltf_animation *anim = &data->animations[ai];
        for (cgltf_size ci = 0; ci < anim->channels_count; ++ci) {
            const cgltf_animation_channel *ch = &anim->channels[ci];
            if (ch->target_path != cgltf_animation_path_type_weights) continue;
            if (!ch->sampler || !ch->target_node || !ch->target_node->mesh)
                continue;

            const cgltf_animation_sampler *samp = ch->sampler;
            if (!samp->input || !samp->output) continue;

            uint32_t num_targets =
                (uint32_t)ch->target_node->mesh->primitives_count > 0
                    ? (uint32_t)ch->target_node->mesh->primitives[0].targets_count
                    : 0;
            if (num_targets == 0) continue;

            uint32_t num_keys = (uint32_t)samp->input->count;
            if (num_keys == 0) continue;

            int32_t node_index = model_node_index_of(data, ch->target_node);
            if (node_index < 0) continue;

            /* cubic-spline output packs [in_tangent, value, out_tangent] per key
             * (3x); extract the value sub-element only and treat as LINEAR
             * (mirrors the joint-channel handling in extract_animations). */
            const int cubic =
                (samp->interpolation == cgltf_interpolation_type_cubic_spline);
            JceMorphInterp interp =
                (samp->interpolation == cgltf_interpolation_type_step)
                    ? JCE_MORPH_INTERP_STEP : JCE_MORPH_INTERP_LINEAR;

            float *ts = (float *)JCE_MALLOC((size_t)num_keys * sizeof(float));
            float *vals = (float *)JCE_MALLOC(
                (size_t)num_keys * num_targets * sizeof(float));
            if (!ts || !vals) { JCE_FREE(ts); JCE_FREE(vals); continue; }

            for (uint32_t k = 0; k < num_keys; ++k)
                cgltf_accessor_read_float(samp->input, k, &ts[k], 1);

            /* Output is (num_keys * num_targets) scalars, key-major.  For
             * cubic-spline each key holds [in_tangent*N, value*N, out_tangent*N]
             * (3N scalars); the value block starts at k*3N + N. */
            for (uint32_t k = 0; k < num_keys; ++k) {
                for (uint32_t t = 0; t < num_targets; ++t) {
                    uint32_t elem = cubic ? (k * 3u * num_targets
                                             + num_targets + t)
                                          : (k * num_targets + t);
                    float w = 0.0f;
                    cgltf_accessor_read_float(samp->output, elem, &w, 1);
                    vals[k * num_targets + t] = w;
                }
            }

            JceMorphWeightTrack *track = jce_morph_weight_track_create(
                num_targets, num_keys, ts, vals, interp);
            JCE_FREE(ts);
            JCE_FREE(vals);
            if (!track) continue;

            out[n].anim_index = (uint32_t)ai;
            out[n].node_index = (uint32_t)node_index;
            out[n].track      = track;
            n++;
        }
    }

    if (n == 0) { JCE_FREE(out); return NULL; }
    *out_count = n;
    return out;
}

/* ================================================================== */
/* Extract nodes                                                       */
/* ================================================================== */

static JceModelNodeCpu *extract_nodes_cpu(cgltf_data *data,
                                          uint32_t *out_count)
{
    /* Count nodes that have meshes. */
    uint32_t count = 0;
    cgltf_size ni;
    for (ni = 0; ni < data->nodes_count; ++ni) {
        if (data->nodes[ni].mesh)
            count++;
    }

    if (count == 0) {
        *out_count = 0;
        return NULL;
    }

    JceModelNodeCpu *nodes = (JceModelNodeCpu *)JCE_CALLOC(count, sizeof(JceModelNodeCpu));
    if (!nodes) { *out_count = 0; return NULL; }

    uint32_t idx = 0;
    for (ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node *gnode = &data->nodes[ni];
        if (!gnode->mesh) continue;

        JceModelNodeCpu *node = &nodes[idx];

        /* Name. */
        if (gnode->name) {
            size_t len = strlen(gnode->name);
            if (len >= sizeof(node->name)) len = sizeof(node->name) - 1;
            memcpy(node->name, gnode->name, len);
            node->name[len] = '\0';
        } else {
            SDL_snprintf(node->name, sizeof(node->name), "node_%u", idx);
        }

        /* World transform: accumulate all ancestor local transforms so the
           draw call just needs to multiply by the model-to-world matrix.
           cgltf_node_transform_world walks the full parent chain for us. */
        cgltf_node_transform_world(gnode, node->local_transform.raw[0]);

        /* Detect if this mesh node is directly parented to a skin joint.
           If so, the static mesh must follow the animated joint instead of
           using the baked bind-pose world transform. */
        node->joint_parent_index = -1;
        node->joint_local_matrix = jce_m4_identity();
        if (data->skins_count > 0 && gnode->parent) {
            const cgltf_skin *skin = &data->skins[0];
            for (cgltf_size ji = 0; ji < skin->joints_count; ji++) {
                if (skin->joints[ji] == gnode->parent) {
                    node->joint_parent_index = (int32_t)ji;
                    /* Store node's local TRS relative to its parent joint.
                       At draw time: world = root × animated_joint_global × joint_local */
                    cgltf_node_transform_local(gnode,
                        node->joint_local_matrix.raw[0]);
                    break;
                }
            }
        }

        /* Parent index: -1 for now (flat list). */
        node->parent = -1;

        /* Morph base weights: node-level weights override mesh-level defaults
         * (glTF 2.0 §3.7.2.2).  Either may be absent. */
        const cgltf_float *base_weights = NULL;
        cgltf_size base_weights_count = 0;
        if (gnode->weights && gnode->weights_count > 0) {
            base_weights = gnode->weights;
            base_weights_count = gnode->weights_count;
        } else if (gnode->mesh->weights && gnode->mesh->weights_count > 0) {
            base_weights = gnode->mesh->weights;
            base_weights_count = gnode->mesh->weights_count;
        }

        /* Primitives → CPU staging. */
        uint32_t num_prims = (uint32_t)gnode->mesh->primitives_count;
        node->num_prims = num_prims;
        node->prims = (JceModelPrimCpu *)JCE_CALLOC(
            num_prims, sizeof(JceModelPrimCpu));
        if (node->prims) {
            uint32_t pi;
            for (pi = 0; pi < num_prims; ++pi) {
                build_primitive_cpu(&gnode->mesh->primitives[pi],
                                    &node->prims[pi], data,
                                    base_weights, base_weights_count);
            }
        }

        idx++;
    }

    *out_count = count;
    return nodes;
}

/* ================================================================== */
/* CPU build (shared by PAK + memory decode)                           */
/* ================================================================== */

/* Build the CPU-only intermediate from a parsed+buffer-loaded cgltf_data.
 * Touches no bgfx → safe on a worker thread. */
static JceModelCpu *build_model_cpu(const JcePakArchive *pak,
                                    const char *path, cgltf_data *data)
{
    JceModelCpu *cpu = (JceModelCpu *)JCE_CALLOC(1, sizeof(JceModelCpu));
    if (!cpu) return NULL;

    cpu->materials   = extract_materials_cpu(pak, path, data, &cpu->num_materials);
    cpu->nodes       = extract_nodes_cpu(data, &cpu->num_nodes);
    cpu->skeleton    = extract_skeleton(data);    /* CPU-only */
    cpu->anim_clips  = extract_animations(data, &cpu->num_anims); /* CPU-only */
    cpu->morph_anims = extract_morph_anims(data, &cpu->num_morph_anims); /* 3.1 */
    return cpu;
}

/* ================================================================== */
/* Worker: decode (no bgfx)                                            */
/* ================================================================== */

/* Derive a PAK-relative forward-slash key from an absolute or mixed-separator
 * path (e.g. "D:/.../resources/assets\models\city\building-b.glb").
 * Writes a normalised copy to buf, then returns a pointer into buf at the
 * start of the relative remainder, or NULL if no safe key can be derived.
 * Only call on the PAK-miss path — cheap string scan, no allocation. */
static const char *pak_derive_relative_key(const char *path,
                                           char *buf, size_t buf_sz)
{
    if (!path || !buf || buf_sz == 0) return NULL;
    size_t L = strlen(path);
    if (L >= buf_sz) return NULL;
    for (size_t i = 0; i <= L; i++)
        buf[i] = (path[i] == '\\') ? '/' : path[i];

    static const char *const s_markers[] = {
        "resources/assets/", "resources/_cooked/", NULL
    };
    static const char *const s_tops[] = {
        "/models/", "/scenes/", "/shaders/", "/fonts/",
        "/i18n/", "/audio/", "/prefabs/", "/anim/",
        "/textures/", NULL
    };
    for (int mi = 0; s_markers[mi]; mi++) {
        const char *p = strstr(buf, s_markers[mi]);
        if (p) { const char *r = p + strlen(s_markers[mi]); return r[0] ? r : NULL; }
    }
    for (int ti = 0; s_tops[ti]; ti++) {
        const char *p = strstr(buf, s_tops[ti]);
        if (p) { const char *r = p + 1; return r[0] ? r : NULL; }
    }
    return NULL;
}

JceModelCpu *jce_gltf_decode_cpu(const JcePakArchive *pak, const char *asset_path)
{
    if (!pak || !asset_path) return NULL;

    /* ---- Decompress from PAK ---- */
    /* pak_path is asset_path or (on absolute-path miss) the derived relative key. */
    const char *pak_path = asset_path;
    char norm_buf[1280];
    const JcePakAsset *asset = jce_pak_find(pak, pak_path);
    if (!asset) {
        /* Belt-and-suspenders: if the exact key missed (e.g. absolute+backslash
         * path saved by the editor), derive a PAK-relative key and retry.
         * Only on the miss path — zero cost for well-formed relative keys. */
        const char *rel = pak_derive_relative_key(asset_path, norm_buf,
                                                  sizeof(norm_buf));
        if (rel) asset = jce_pak_find(pak, rel);
        if (asset)
            pak_path = rel;
        else {
            LOG_ERROR(LOG_TAG, "model not found in PAK: %s", asset_path);
            return NULL;
        }
    }

    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) return NULL;

    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", pak_path);
        JCE_FREE(buf);
        return NULL;
    }

    /* ---- Parse with cgltf ---- */
    cgltf_options options;
    memset(&options, 0, sizeof(options));
    JceGltfPakFileCtx file_ctx = { pak };
    options.file.read = jce_gltf_pak_file_read;
    options.file.release = jce_gltf_pak_file_release;
    options.file.user_data = &file_ctx;

    cgltf_data *data = NULL;
    cgltf_result result = cgltf_parse(&options, buf, (cgltf_size)n, &data);
    if (result != cgltf_result_success) {
        LOG_ERROR(LOG_TAG, "cgltf_parse failed (%d): %s", (int)result, pak_path);
        JCE_FREE(buf);
        return NULL;
    }

    /* For GLB, binary data is inline; for glTF, cgltf resolves external
     * buffers relative to pak_path and reads them through the PAK callback. */
    result = cgltf_load_buffers(&options, data, pak_path);
    if (result != cgltf_result_success) {
        LOG_ERROR(LOG_TAG, "cgltf_load_buffers failed (%d): %s",
                  (int)result, pak_path);
        cgltf_free(data);
        JCE_FREE(buf);
        return NULL;
    }

    /* Safety: ensure buffers[0].data links to data->bin even if cgltf's size
       check produced a silent mismatch, so embedded textures can decode. */
    if (data->buffers_count > 0 && !data->buffers[0].data && data->bin) {
        data->buffers[0].data = (void *)data->bin;
        data->buffers[0].size = data->bin_size;
    }

    /* Validate accessor / buffer-view ranges before any accessor read.  cgltf
     * only cross-checks accessor offset+stride*count against the buffer view
     * inside cgltf_validate(); without it a crafted glTF (count >> buffer)
     * drives out-of-bounds heap reads in build_model_cpu and overflows 32-bit
     * allocation math (audit Round-3 P1; also subsumes the F46 cubic-spline
     * index hazard).  The bin-link fixup above runs first so legit GLBs with
     * cgltf's silent size quirk still validate. */
    result = cgltf_validate(data);
    if (result != cgltf_result_success) {
        LOG_ERROR(LOG_TAG, "cgltf_validate failed (%d): %s — refusing malformed glTF",
                  (int)result, pak_path);
        cgltf_free(data);
        JCE_FREE(buf);
        return NULL;
    }

    JceModelCpu *cpu = build_model_cpu(pak, pak_path, data);
    if (cpu) {
        LOG_DEBUG(LOG_TAG, "decoded %s: %u nodes, %u materials, %u anims%s",
                  pak_path, cpu->num_nodes, cpu->num_materials,
                  cpu->num_anims, cpu->skeleton ? " (skinned)" : "");
    }

    cgltf_free(data);
    JCE_FREE(buf);
    return cpu;
}

JceModelCpu *jce_gltf_decode_cpu_memory(const void *file_data, uint32_t size,
                                        const char *name)
{
    if (!file_data || size == 0) return NULL;
    const char *tag = name ? name : "<memory>";

    cgltf_options options;
    memset(&options, 0, sizeof(options));
    cgltf_data *data = NULL;
    cgltf_result result = cgltf_parse(&options, file_data, (cgltf_size)size,
                                      &data);
    if (result != cgltf_result_success) {
        LOG_ERROR(LOG_TAG, "cgltf_parse failed (%d): %s", (int)result, tag);
        return NULL;
    }

    /* `name` is the resolved filesystem path when the caller has one (lets
     * cgltf's default reader open a sibling external .bin); for embedded/.glb
     * data it is just a tag and no external read happens. */
    result = cgltf_load_buffers(&options, data, name);
    if (result != cgltf_result_success) {
        LOG_ERROR(LOG_TAG, "cgltf_load_buffers failed (%d): %s",
                  (int)result, tag);
        cgltf_free(data);
        return NULL;
    }

    if (data->buffers_count > 0 && !data->buffers[0].data && data->bin) {
        data->buffers[0].data = (void *)data->bin;
        data->buffers[0].size = data->bin_size;
    }

    /* Validate accessor / buffer-view ranges before any accessor read — this
     * entry accepts arbitrary (untrusted) memory, so a malformed glTF must be
     * rejected rather than driving OOB reads / 32-bit alloc overflow in
     * build_model_cpu (audit Round-3 P1; subsumes the F46 cubic hazard). */
    result = cgltf_validate(data);
    if (result != cgltf_result_success) {
        LOG_ERROR(LOG_TAG, "cgltf_validate failed (%d): %s — refusing malformed glTF",
                  (int)result, tag);
        cgltf_free(data);
        return NULL;
    }

    /* No PAK so textures resolve via embedded data / host disk fallback. */
    JceModelCpu *cpu = build_model_cpu(NULL, tag, data);
    if (cpu) {
        LOG_DEBUG(LOG_TAG, "decoded %s (memory): %u nodes, %u materials, %u anims%s",
                  tag, cpu->num_nodes, cpu->num_materials,
                  cpu->num_anims, cpu->skeleton ? " (skinned)" : "");
    }

    cgltf_free(data);
    return cpu;
}

/* ================================================================== */
/* Lightweight rig probe (header-only, no geometry / buffers / GPU)   */
/* ================================================================== */

bool jce_gltf_probe_rig_memory(const void *file_data, uint32_t size,
                               bool *out_has_skin, bool *out_has_anim)
{
    if (out_has_skin) *out_has_skin = false;
    if (out_has_anim) *out_has_anim = false;
    if (!file_data || size == 0) return false;

    /* cgltf_parse reads only the JSON header (and, for GLB, the chunk
     * table) — it does NOT load buffers, decode images, build meshes, or
     * sample animation tracks.  That makes this orders of magnitude
     * cheaper than a full load: just enough to answer "is this rigged?"
     * for the editor's drop path without blocking the main thread. */
    cgltf_options options;
    memset(&options, 0, sizeof(options));
    cgltf_data *data = NULL;
    if (cgltf_parse(&options, file_data, (cgltf_size)size, &data)
            != cgltf_result_success) {
        return false;
    }

    if (out_has_skin) *out_has_skin = data->skins_count > 0;
    if (out_has_anim) *out_has_anim = data->animations_count > 0;

    cgltf_free(data);
    return true;
}

/* ================================================================== */
/* Render thread: upload (bgfx) + free the CPU intermediate           */
/* ================================================================== */

JceModel *jce_gltf_upload_cpu(JceModelCpu *cpu)
{
    if (!cpu) return NULL;

    JceModel *model = (JceModel *)JCE_CALLOC(1, sizeof(JceModel));
    if (!model) { jce_gltf_model_cpu_free(cpu); return NULL; }

    /* Local-space AABB seed (model space); accumulated over every primitive's
     * vertices × its node's baked world transform in the node loop below, so the
     * scene renderer can frustum-cull this model by its true extent instead of a
     * point at the origin (fixes large/un-scaled models vanishing when their
     * centre leaves the view). */
    model->aabb_min[0] = model->aabb_min[1] = model->aabb_min[2] =  FLT_MAX;
    model->aabb_max[0] = model->aabb_max[1] = model->aabb_max[2] = -FLT_MAX;
    model->has_aabb = false;

    /* Materials: copy factors, upload each decoded map (consumes texcpu). */
    if (cpu->num_materials > 0 && cpu->materials) {
        model->materials = (JcePbrMaterial *)JCE_CALLOC(
            cpu->num_materials, sizeof(JcePbrMaterial));
        if (model->materials) {
            model->num_materials = cpu->num_materials;
            for (uint32_t i = 0; i < cpu->num_materials; ++i) {
                JceModelMatCpu *mc = &cpu->materials[i];
                JcePbrMaterial *dst = &model->materials[i];
                *dst = mc->base;
                if (mc->albedo)   { dst->albedo_map = jce_texture_upload_cpu(mc->albedo);   mc->albedo = NULL; }
                if (mc->mr)       { dst->metallic_roughness_map = jce_texture_upload_cpu(mc->mr); mc->mr = NULL; }
                if (mc->normal)   { dst->normal_map = jce_texture_upload_cpu(mc->normal);   mc->normal = NULL; }
                if (mc->ao)       { dst->ao_map = jce_texture_upload_cpu(mc->ao);           mc->ao = NULL; }
                if (mc->emissive) { dst->emissive_map = jce_texture_upload_cpu(mc->emissive); mc->emissive = NULL; }
            }
        }
    }

    /* Nodes: create GPU meshes from the CPU vertex/index arrays. */
    if (cpu->num_nodes > 0 && cpu->nodes) {
        model->nodes = (JceModelNode *)JCE_CALLOC(cpu->num_nodes, sizeof(JceModelNode));
        if (model->nodes) {
            model->num_nodes = cpu->num_nodes;
            for (uint32_t n = 0; n < cpu->num_nodes; ++n) {
                JceModelNodeCpu *src = &cpu->nodes[n];
                JceModelNode    *dn  = &model->nodes[n];
                memcpy(dn->name, src->name, sizeof(dn->name));
                dn->local_transform    = src->local_transform;
                dn->parent             = src->parent;
                dn->joint_parent_index = src->joint_parent_index;
                dn->joint_local_matrix = src->joint_local_matrix;
                dn->num_primitives     = src->num_prims;
                if (src->num_prims > 0) {
                    dn->primitives = (JceModelPrimitive *)JCE_CALLOC(
                        src->num_prims, sizeof(JceModelPrimitive));
                    if (dn->primitives) {
                        for (uint32_t p = 0; p < src->num_prims; ++p) {
                            JceModelPrimCpu  *sp = &src->prims[p];
                            JceModelPrimitive *dp = &dn->primitives[p];
                            dp->material_index = sp->material_index;
                            dp->static_mesh    = NULL;
                            /* Morph deltas: transfer ownership (CPU-only, no
                             * GPU resource).  FEATURE 3.1 CPU pre-skin path. */
                            dp->morph          = sp->morph; sp->morph = NULL;
                            if (!sp->verts || sp->num_verts == 0) continue;
                            /* Accumulate this primitive's vertex positions
                             * (node-local) transformed by the node's baked world
                             * matrix into the model-space AABB.  pos[3] is the
                             * first field of both JcePbrVertex and JceSkinnedVertex. */
                            {
                                size_t vstride = (sp->kind == 1)
                                    ? sizeof(JceSkinnedVertex) : sizeof(JcePbrVertex);
                                const char *vb = (const char *)sp->verts;
                                for (uint32_t k = 0; k < sp->num_verts; ++k) {
                                    const float *pp =
                                        (const float *)(const void *)(vb + (size_t)k * vstride);
                                    jce_vec4 lv = { pp[0], pp[1], pp[2], 1.0f };
                                    jce_vec4 wv = jce_m4_mul_v4(&src->local_transform, lv);
                                    if (wv.x < model->aabb_min[0]) model->aabb_min[0] = wv.x;
                                    if (wv.y < model->aabb_min[1]) model->aabb_min[1] = wv.y;
                                    if (wv.z < model->aabb_min[2]) model->aabb_min[2] = wv.z;
                                    if (wv.x > model->aabb_max[0]) model->aabb_max[0] = wv.x;
                                    if (wv.y > model->aabb_max[1]) model->aabb_max[1] = wv.y;
                                    if (wv.z > model->aabb_max[2]) model->aabb_max[2] = wv.z;
                                }
                                model->has_aabb = true;
                            }
                            /* FEATURE 3.1: retain an undeformed CPU copy of the
                             * base verts ONLY for morph-bearing prims, so the
                             * per-instance GPU morph deform can re-upload into a
                             * dynamic VB.  Non-morph prims keep retain=false =>
                             * zero RAM regression for ordinary characters. */
                            bool retain_morph = (dp->morph != NULL);
                            if (sp->kind == 1)
                                dp->skinned_mesh = jce_skinned_mesh_create(
                                    (const JceSkinnedVertex *)sp->verts, sp->num_verts,
                                    sp->indices, sp->num_indices, retain_morph);
                            else
                                dp->skinned_mesh = jce_pbr_mesh_create(
                                    (const JcePbrVertex *)sp->verts, sp->num_verts,
                                    sp->indices, sp->num_indices, retain_morph);
                            /* In-asset auto-LOD (P1 #6): upload each parsed
                             * reduced index set as an alternate IBO sharing this
                             * mesh's vertex buffer.  Only the static (non-
                             * skinned) path carries LODs — skinned rigs keep full
                             * detail (sp->kind == 0 prims are the only ones whose
                             * JCE_lod the cook emits).  No-op when lod_count==0. */
                            if (dp->skinned_mesh && sp->kind == 0) {
                                for (uint32_t li = 0; li < sp->lod_count; ++li)
                                    jce_skinned_mesh_add_lod(
                                        dp->skinned_mesh,
                                        sp->lod_indices[li],
                                        sp->lod_num_indices[li]);
                                /* Nanite-lite V1: meshlet sidecar (static
                                 * prims only, mirrors the LOD upload). */
                                if (sp->ml_count)
                                    jce_skinned_mesh_set_meshlets(
                                        dp->skinned_mesh,
                                        sp->ml_indices, sp->ml_num_indices,
                                        sp->ml_desc, sp->ml_bounds,
                                        sp->ml_errors, sp->ml_count);
                            }
                        }
                    }
                }
            }
        }
    }

    /* Skeleton + animations: transfer ownership (already CPU-built). */
    model->skeleton   = cpu->skeleton;   cpu->skeleton = NULL;
    model->anim_clips = cpu->anim_clips; cpu->anim_clips = NULL;
    model->num_anims  = cpu->num_anims;  cpu->num_anims = 0;

    /* Morph-weight tracks (FEATURE 3.1): transfer ownership.  Re-pack the CPU
     * staging array into the model's JceModelMorphAnim array (tracks are
     * CPU-only — no GPU resource). */
    if (cpu->num_morph_anims > 0 && cpu->morph_anims) {
        model->morph_anims = (JceModelMorphAnim *)JCE_CALLOC(
            cpu->num_morph_anims, sizeof(JceModelMorphAnim));
        if (model->morph_anims) {
            model->num_morph_anims = cpu->num_morph_anims;
            for (uint32_t i = 0; i < cpu->num_morph_anims; ++i) {
                model->morph_anims[i].anim_index = cpu->morph_anims[i].anim_index;
                model->morph_anims[i].node_index = cpu->morph_anims[i].node_index;
                model->morph_anims[i].track      = cpu->morph_anims[i].track;
                cpu->morph_anims[i].track = NULL;  /* ownership moved */
            }
        }
    }

    /* Free the CPU intermediate (textures + skeleton/anims already moved out;
     * jce_gltf_model_cpu_free is null-safe per remaining field). */
    jce_gltf_model_cpu_free(cpu);
    return model;
}

void jce_gltf_model_cpu_free(JceModelCpu *cpu)
{
    if (!cpu) return;

    if (cpu->nodes) {
        for (uint32_t n = 0; n < cpu->num_nodes; ++n) {
            JceModelNodeCpu *nd = &cpu->nodes[n];
            if (nd->prims) {
                for (uint32_t p = 0; p < nd->num_prims; ++p) {
                    if (nd->prims[p].verts)   JCE_FREE(nd->prims[p].verts);
                    if (nd->prims[p].indices) JCE_FREE(nd->prims[p].indices);
                    for (uint32_t li = 0; li < nd->prims[p].lod_count; ++li)
                        if (nd->prims[p].lod_indices[li])
                            JCE_FREE(nd->prims[p].lod_indices[li]);
                    if (nd->prims[p].ml_indices) JCE_FREE(nd->prims[p].ml_indices);
                    if (nd->prims[p].ml_desc)    JCE_FREE(nd->prims[p].ml_desc);
                    if (nd->prims[p].ml_bounds)  JCE_FREE(nd->prims[p].ml_bounds);
                    if (nd->prims[p].ml_errors)  JCE_FREE(nd->prims[p].ml_errors);
                    if (nd->prims[p].morph)
                        jce_morph_data_destroy(nd->prims[p].morph);
                }
                JCE_FREE(nd->prims);
            }
        }
        JCE_FREE(cpu->nodes);
    }

    if (cpu->materials) {
        for (uint32_t i = 0; i < cpu->num_materials; ++i) {
            JceModelMatCpu *m = &cpu->materials[i];
            jce_texture_cpu_free(m->albedo);
            jce_texture_cpu_free(m->mr);
            jce_texture_cpu_free(m->normal);
            jce_texture_cpu_free(m->ao);
            jce_texture_cpu_free(m->emissive);
        }
        JCE_FREE(cpu->materials);
    }

    if (cpu->skeleton) jce_skeleton_destroy(cpu->skeleton);
    if (cpu->anim_clips) {
        for (uint32_t i = 0; i < cpu->num_anims; ++i)
            jce_anim_clip_destroy(cpu->anim_clips[i]);
        JCE_FREE(cpu->anim_clips);
    }

    if (cpu->morph_anims) {
        for (uint32_t i = 0; i < cpu->num_morph_anims; ++i)
            jce_morph_weight_track_destroy(cpu->morph_anims[i].track);
        JCE_FREE(cpu->morph_anims);
    }

    JCE_FREE(cpu);
}

/* ================================================================== */
/* Morph-target inspection on the CPU intermediate (FEATURE 3.1)        */
/* ================================================================== */

uint32_t jce_gltf_cpu_node_count(const JceModelCpu *cpu)
{
    return cpu ? cpu->num_nodes : 0;
}

uint32_t jce_gltf_cpu_node_prim_count(const JceModelCpu *cpu, uint32_t node)
{
    if (!cpu || node >= cpu->num_nodes || !cpu->nodes) return 0;
    return cpu->nodes[node].num_prims;
}

const struct JceMorphData *jce_gltf_cpu_prim_morph(const JceModelCpu *cpu,
                                                   uint32_t node, uint32_t prim)
{
    if (!cpu || node >= cpu->num_nodes || !cpu->nodes) return NULL;
    const JceModelNodeCpu *nd = &cpu->nodes[node];
    if (prim >= nd->num_prims || !nd->prims) return NULL;
    return nd->prims[prim].morph;
}

uint32_t jce_gltf_cpu_morph_anim_count(const JceModelCpu *cpu)
{
    return cpu ? cpu->num_morph_anims : 0;
}

const struct JceMorphWeightTrack *jce_gltf_cpu_morph_anim_track(
    const JceModelCpu *cpu, uint32_t index,
    uint32_t *out_anim_index, uint32_t *out_node_index)
{
    if (!cpu || index >= cpu->num_morph_anims || !cpu->morph_anims) return NULL;
    const JceModelMorphAnimCpu *ma = &cpu->morph_anims[index];
    if (out_anim_index) *out_anim_index = ma->anim_index;
    if (out_node_index) *out_node_index = ma->node_index;
    return ma->track;
}

/* ── Skeleton / skin / anim inspection on the CPU intermediate ─────────
 * Lets a headless caller (e.g. the FBX skinned-import unit test) assert that a
 * decoded-but-not-uploaded JceModelCpu carries a skeleton, skinned vertices, and
 * animation clips WITHOUT a GPU context (the upload step is what needs bgfx). */

const JceSkeleton *jce_gltf_cpu_skeleton(const JceModelCpu *cpu)
{
    return cpu ? cpu->skeleton : NULL;
}

uint32_t jce_gltf_cpu_anim_count(const JceModelCpu *cpu)
{
    return cpu ? cpu->num_anims : 0;
}

const JceAnimClip *jce_gltf_cpu_anim_clip(const JceModelCpu *cpu, uint32_t index)
{
    if (!cpu || index >= cpu->num_anims || !cpu->anim_clips) return NULL;
    return cpu->anim_clips[index];
}

/* Is (node, prim) a skinned primitive?  (kind == 1) */
bool jce_gltf_cpu_prim_is_skinned(const JceModelCpu *cpu,
                                  uint32_t node, uint32_t prim)
{
    if (!cpu || node >= cpu->num_nodes || !cpu->nodes) return false;
    const JceModelNodeCpu *nd = &cpu->nodes[node];
    if (prim >= nd->num_prims || !nd->prims) return false;
    return nd->prims[prim].kind == 1;
}

uint32_t jce_gltf_cpu_prim_vertex_count(const JceModelCpu *cpu,
                                        uint32_t node, uint32_t prim)
{
    if (!cpu || node >= cpu->num_nodes || !cpu->nodes) return 0;
    const JceModelNodeCpu *nd = &cpu->nodes[node];
    if (prim >= nd->num_prims || !nd->prims) return 0;
    return nd->prims[prim].num_verts;
}

/* Read the bone weights of skinned vertex `vtx` of (node, prim) into out_w[4].
 * Returns false if the prim is not skinned or indices are out of range. */
bool jce_gltf_cpu_prim_skinned_weights(const JceModelCpu *cpu,
                                       uint32_t node, uint32_t prim,
                                       uint32_t vtx, float out_w[4])
{
    if (!cpu || node >= cpu->num_nodes || !cpu->nodes) return false;
    const JceModelNodeCpu *nd = &cpu->nodes[node];
    if (prim >= nd->num_prims || !nd->prims) return false;
    const JceModelPrimCpu *p = &nd->prims[prim];
    if (p->kind != 1 || !p->verts || vtx >= p->num_verts) return false;
    const JceSkinnedVertex *v = &((const JceSkinnedVertex *)p->verts)[vtx];
    out_w[0] = v->weights[0]; out_w[1] = v->weights[1];
    out_w[2] = v->weights[2]; out_w[3] = v->weights[3];
    return true;
}

/* ================================================================== */
/* Generic CPU-model builder (format-agnostic skinned import)          */
/* ------------------------------------------------------------------ */
/* The struct JceModelCpu intermediate + its GPU upload (jce_gltf_upload_cpu)
 * live HERE in the renderer layer.  Importers in OTHER layers (e.g. the assimp
 * FBX path in jce_resource) cannot allocate/populate the opaque JceModelCpu
 * directly, so this small builder lets them hand over already-extracted CPU
 * geometry / skeleton / clips.  Everything below is bgfx-free (it only allocates
 * CPU arrays and calls the animation layer's CPU-only skeleton/clip creators),
 * so a caller — or a headless unit test — can build a JceModelCpu without a GPU
 * context and assert on it via the jce_gltf_cpu_* accessors, then upload later
 * with jce_gltf_upload_cpu() on the render thread (which is where the bgfx mesh
 * creation happens).  Ownership of skeleton/clips passed to the setters moves
 * into the builder; vertex/index arrays are COPIED. */

JceModelCpu *jce_model_cpu_builder_create(void)
{
    return (JceModelCpu *)JCE_CALLOC(1, sizeof(JceModelCpu));
}

/* Reserve `num_nodes` mesh-bearing nodes.  Each node holds one primitive.
 * Returns false on OOM or if nodes were already reserved. */
bool jce_model_cpu_builder_reserve_nodes(JceModelCpu *cpu, uint32_t num_nodes)
{
    if (!cpu || cpu->nodes) return false;
    if (num_nodes == 0) return true;
    cpu->nodes = (JceModelNodeCpu *)JCE_CALLOC(num_nodes, sizeof(JceModelNodeCpu));
    if (!cpu->nodes) return false;
    cpu->num_nodes = num_nodes;
    return true;
}

/* Populate node `node_index` with ONE skinned primitive built from a COPY of
 * the supplied JceSkinnedVertex array + index array.  `name` may be NULL.
 * `local_transform` is the node's model-space transform (column-major).  The
 * skinned bone palette is addressed through the model's skeleton, so the
 * vertices' joints[] must already be skeleton joint indices. */
bool jce_model_cpu_builder_set_skinned_node(JceModelCpu *cpu, uint32_t node_index,
                                            const char *name,
                                            const jce_mat4 *local_transform,
                                            const JceSkinnedVertex *verts,
                                            uint32_t num_verts,
                                            const uint32_t *indices,
                                            uint32_t num_indices,
                                            uint32_t material_index)
{
    if (!cpu || node_index >= cpu->num_nodes || !cpu->nodes) return false;
    if (!verts || num_verts == 0) return false;

    JceModelNodeCpu *node = &cpu->nodes[node_index];

    if (name && name[0]) {
        size_t len = strlen(name);
        if (len >= sizeof(node->name)) len = sizeof(node->name) - 1;
        memcpy(node->name, name, len);
        node->name[len] = '\0';
    } else {
        SDL_snprintf(node->name, sizeof(node->name), "node_%u", node_index);
    }
    node->local_transform    = local_transform ? *local_transform : jce_m4_identity();
    node->parent             = -1;
    node->joint_parent_index = -1;
    node->joint_local_matrix = jce_m4_identity();

    node->prims = (JceModelPrimCpu *)JCE_CALLOC(1, sizeof(JceModelPrimCpu));
    if (!node->prims) return false;
    node->num_prims = 1;

    JceModelPrimCpu *p = &node->prims[0];
    p->kind           = 1;   /* skinned */
    p->material_index = material_index;

    JceSkinnedVertex *vcopy =
        (JceSkinnedVertex *)JCE_MALLOC((size_t)num_verts * sizeof(JceSkinnedVertex));
    if (!vcopy) return false;
    memcpy(vcopy, verts, (size_t)num_verts * sizeof(JceSkinnedVertex));
    p->verts     = vcopy;
    p->num_verts = num_verts;

    if (indices && num_indices > 0) {
        uint32_t *icopy =
            (uint32_t *)JCE_MALLOC((size_t)num_indices * sizeof(uint32_t));
        if (!icopy) { JCE_FREE(vcopy); p->verts = NULL; return false; }
        memcpy(icopy, indices, (size_t)num_indices * sizeof(uint32_t));
        p->indices     = icopy;
        p->num_indices = num_indices;
    }
    return true;
}

/* Populate node `node_index` with ONE static-PBR primitive (tangents, no
 * skinning) from a COPY of the supplied JcePbrVertex array. */
bool jce_model_cpu_builder_set_static_node(JceModelCpu *cpu, uint32_t node_index,
                                           const char *name,
                                           const jce_mat4 *local_transform,
                                           const JcePbrVertex *verts,
                                           uint32_t num_verts,
                                           const uint32_t *indices,
                                           uint32_t num_indices,
                                           uint32_t material_index)
{
    if (!cpu || node_index >= cpu->num_nodes || !cpu->nodes) return false;
    if (!verts || num_verts == 0) return false;

    JceModelNodeCpu *node = &cpu->nodes[node_index];

    if (name && name[0]) {
        size_t len = strlen(name);
        if (len >= sizeof(node->name)) len = sizeof(node->name) - 1;
        memcpy(node->name, name, len);
        node->name[len] = '\0';
    } else {
        SDL_snprintf(node->name, sizeof(node->name), "node_%u", node_index);
    }
    node->local_transform    = local_transform ? *local_transform : jce_m4_identity();
    node->parent             = -1;
    node->joint_parent_index = -1;
    node->joint_local_matrix = jce_m4_identity();

    node->prims = (JceModelPrimCpu *)JCE_CALLOC(1, sizeof(JceModelPrimCpu));
    if (!node->prims) return false;
    node->num_prims = 1;

    JceModelPrimCpu *p = &node->prims[0];
    p->kind           = 0;   /* static PBR */
    p->material_index = material_index;

    JcePbrVertex *vcopy =
        (JcePbrVertex *)JCE_MALLOC((size_t)num_verts * sizeof(JcePbrVertex));
    if (!vcopy) return false;
    memcpy(vcopy, verts, (size_t)num_verts * sizeof(JcePbrVertex));
    p->verts     = vcopy;
    p->num_verts = num_verts;

    if (indices && num_indices > 0) {
        uint32_t *icopy =
            (uint32_t *)JCE_MALLOC((size_t)num_indices * sizeof(uint32_t));
        if (!icopy) { JCE_FREE(vcopy); p->verts = NULL; return false; }
        memcpy(icopy, indices, (size_t)num_indices * sizeof(uint32_t));
        p->indices     = icopy;
        p->num_indices = num_indices;
    }
    return true;
}

/* Move ownership of an already-built skeleton into the model (replaces any
 * existing one, which is destroyed). */
void jce_model_cpu_builder_set_skeleton(JceModelCpu *cpu, JceSkeleton *skel)
{
    if (!cpu) return;
    if (cpu->skeleton && cpu->skeleton != skel)
        jce_skeleton_destroy(cpu->skeleton);
    cpu->skeleton = skel;
}

/* Move ownership of an animation-clip array (clips[] and each clip) into the
 * model.  `clips` must be a JCE_MALLOC'd array of `count` owned JceAnimClip*. */
void jce_model_cpu_builder_set_anims(JceModelCpu *cpu,
                                     JceAnimClip **clips, uint32_t count)
{
    if (!cpu) return;
    if (cpu->anim_clips) {
        for (uint32_t i = 0; i < cpu->num_anims; ++i)
            jce_anim_clip_destroy(cpu->anim_clips[i]);
        JCE_FREE(cpu->anim_clips);
    }
    cpu->anim_clips = clips;
    cpu->num_anims  = clips ? count : 0;
}

/* Install a single default material (factors only, no textures) so the upload
 * path always has at least one material to index.  Importers that extract real
 * materials can build cpu->materials themselves; this is the minimal default. */
bool jce_model_cpu_builder_set_default_material(JceModelCpu *cpu)
{
    if (!cpu || cpu->materials) return false;
    cpu->materials = (JceModelMatCpu *)JCE_CALLOC(1, sizeof(JceModelMatCpu));
    if (!cpu->materials) return false;
    cpu->materials[0].base = jce_pbr_material_default();
    cpu->num_materials = 1;
    return true;
}

/* ================================================================== */
/* Sync wrappers (decode + upload)                                    */
/* ================================================================== */

JceModel *jce_gltf_load(const JcePakArchive *pak, const char *asset_path)
{
    return jce_gltf_upload_cpu(jce_gltf_decode_cpu(pak, asset_path));
}

JceModel *jce_gltf_load_memory(const void *file_data, uint32_t size,
                               const char *name)
{
    return jce_gltf_upload_cpu(jce_gltf_decode_cpu_memory(file_data, size, name));
}
