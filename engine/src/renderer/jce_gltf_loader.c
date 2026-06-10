/*
 * jce_gltf_loader.c  glTF 2.0 / GLB model loading from PAK.
 *
 * Uses cgltf (single-header C99 glTF parser) to extract meshes,
 * PBR materials, textures, skeleton, and animation clips from a
 * GLB binary blob decompressed from the PAK archive.
 */

#include "jce_gltf_loader.h"

#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/renderer/jce_skinned_mesh.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_texture.h>

#include "middleware/animation/jce_animation.h"
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

/* One primitive's geometry as CPU arrays (no GPU buffers yet). */
typedef struct {
    int       kind;            /* 0 = static PBR (JcePbrVertex), 1 = skinned */
    void     *verts;           /* owned: JcePbrVertex[] or JceSkinnedVertex[] */
    uint32_t  num_verts;
    uint32_t *indices;         /* owned, may be NULL */
    uint32_t  num_indices;
    uint32_t  material_index;
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

struct JceModelCpu {
    JceModelNodeCpu *nodes;      uint32_t num_nodes;
    JceModelMatCpu  *materials;  uint32_t num_materials;
    JceSkeleton     *skeleton;   /* CPU-only, built on the worker */
    JceAnimClip    **anim_clips; uint32_t num_anims;
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

/* Extract one glTF primitive's geometry into CPU staging arrays (no GPU
 * buffers; jce_gltf_upload_cpu creates them later).  Ownership of verts +
 * indices transfers to `out`. */
static void build_primitive_cpu(const cgltf_primitive *prim,
                                JceModelPrimCpu *out,
                                const cgltf_data *data)
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
                continue; /* skip weights etc. */
            }

            /* Interpolation. */
            switch (samp->interpolation) {
            case cgltf_interpolation_type_step:
                dst->interpolation = JCE_INTERP_STEP;
                break;
            case cgltf_interpolation_type_cubic_spline:
                dst->interpolation = JCE_INTERP_CUBIC_SPLINE;
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
                    cgltf_accessor_read_float(samp->output, ki, v, 3);
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
                    cgltf_accessor_read_float(samp->output, ki, v, 4);
                    vals[ki].x = v[0];
                    vals[ki].y = v[1];
                    vals[ki].z = v[2];
                    vals[ki].w = v[3];
                }
                dst->rotations = vals;
            }

            valid_channels++;
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

        /* Primitives → CPU staging. */
        uint32_t num_prims = (uint32_t)gnode->mesh->primitives_count;
        node->num_prims = num_prims;
        node->prims = (JceModelPrimCpu *)JCE_CALLOC(
            num_prims, sizeof(JceModelPrimCpu));
        if (node->prims) {
            uint32_t pi;
            for (pi = 0; pi < num_prims; ++pi) {
                build_primitive_cpu(&gnode->mesh->primitives[pi],
                                    &node->prims[pi], data);
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

    cpu->materials  = extract_materials_cpu(pak, path, data, &cpu->num_materials);
    cpu->nodes      = extract_nodes_cpu(data, &cpu->num_nodes);
    cpu->skeleton   = extract_skeleton(data);    /* CPU-only */
    cpu->anim_clips = extract_animations(data, &cpu->num_anims); /* CPU-only */
    return cpu;
}

/* ================================================================== */
/* Worker: decode (no bgfx)                                            */
/* ================================================================== */

JceModelCpu *jce_gltf_decode_cpu(const JcePakArchive *pak, const char *asset_path)
{
    if (!pak || !asset_path) return NULL;

    /* ---- Decompress from PAK ---- */
    const JcePakAsset *asset = jce_pak_find(pak, asset_path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "model not found in PAK: %s", asset_path);
        return NULL;
    }

    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) return NULL;

    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", asset_path);
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
        LOG_ERROR(LOG_TAG, "cgltf_parse failed (%d): %s", (int)result, asset_path);
        JCE_FREE(buf);
        return NULL;
    }

    /* For GLB, binary data is inline; for glTF, cgltf resolves external
     * buffers relative to asset_path and reads them through the PAK callback. */
    result = cgltf_load_buffers(&options, data, asset_path);
    if (result != cgltf_result_success) {
        LOG_ERROR(LOG_TAG, "cgltf_load_buffers failed (%d): %s",
                  (int)result, asset_path);
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

    JceModelCpu *cpu = build_model_cpu(pak, asset_path, data);
    if (cpu) {
        LOG_DEBUG(LOG_TAG, "decoded %s: %u nodes, %u materials, %u anims%s",
                  asset_path, cpu->num_nodes, cpu->num_materials,
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
/* Render thread: upload (bgfx) + free the CPU intermediate           */
/* ================================================================== */

JceModel *jce_gltf_upload_cpu(JceModelCpu *cpu)
{
    if (!cpu) return NULL;

    JceModel *model = (JceModel *)JCE_CALLOC(1, sizeof(JceModel));
    if (!model) { jce_gltf_model_cpu_free(cpu); return NULL; }

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
                            if (!sp->verts || sp->num_verts == 0) continue;
                            if (sp->kind == 1)
                                dp->skinned_mesh = jce_skinned_mesh_create(
                                    (const JceSkinnedVertex *)sp->verts, sp->num_verts,
                                    sp->indices, sp->num_indices);
                            else
                                dp->skinned_mesh = jce_pbr_mesh_create(
                                    (const JcePbrVertex *)sp->verts, sp->num_verts,
                                    sp->indices, sp->num_indices);
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

    JCE_FREE(cpu);
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
