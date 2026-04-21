/*
 * jce_gltf_loader.c  glTF 2.0 / GLB model loading from PAK.
 *
 * Uses cgltf (single-header C99 glTF parser) to extract meshes,
 * PBR materials, textures, skeleton, and animation clips from a
 * GLB binary blob decompressed from the PAK archive.
 */

#include <cgltf.h>

#include "jce_gltf_loader.h"
#include <jce/core/pak_loader.h>
#include "graphics/jce_model_internal.h"
#include <jce/graphics/jce_texture.h>
#include <jce/graphics/jce_mesh.h>
#include <jce/animation/jce_skinned_mesh.h>
#include <jce/animation/jce_skeleton.h>
#include "animation/jce_animation.h"
#include <jce/core/jce_log.h>
#include <jce/core/jce_math.h>
#include "core/jce_memory.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "jce_gltf"

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

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

/* Load a texture from a glTF image (either URI or embedded buffer_view). */
static JceTexture load_gltf_texture(const JcePakArchive *pak,
                                     const char *model_path,
                                     const cgltf_image *image,
                                     cgltf_data *data)
{
    if (!image) return JCE_TEXTURE_INVALID;

    /* Case 1: embedded texture via buffer_view (common in GLB). */
    if (image->buffer_view) {
        const cgltf_buffer_view *bv = image->buffer_view;
        if (bv->buffer && bv->buffer->data) {
            const uint8_t *img_data = (const uint8_t *)bv->buffer->data
                                      + bv->offset;
            cgltf_size img_size = bv->size;

            /* Decode with SDL3_image via IOStream. */
            SDL_IOStream *io = SDL_IOFromConstMem(img_data, (size_t)img_size);
            if (!io) return JCE_TEXTURE_INVALID;

            SDL_Surface *surf = IMG_Load_IO(io, true);
            if (!surf) {
                LOG_ERROR(LOG_TAG, "failed to decode embedded texture");
                return JCE_TEXTURE_INVALID;
            }

            /* Ensure RGBA32 so jce_texture_load_from_surface gets tightly
               packed 4-byte pixels regardless of the source PNG colour mode. */
            if (surf->format != SDL_PIXELFORMAT_RGBA32) {
                SDL_Surface *conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
                SDL_DestroySurface(surf);
                if (!conv) {
                    LOG_ERROR(LOG_TAG, "surface convert failed: %s", SDL_GetError());
                    return JCE_TEXTURE_INVALID;
                }
                surf = conv;
            }

            JceTexture tex = jce_texture_load_from_surface(surf, JCE_TEX_WRAP);
            SDL_DestroySurface(surf);
            return tex;
        }
    }

    /* Case 2: external URI -- resolve relative to model path. Try PAK first
     * (runtime), then fall back to disk (editor / dev workflow). */
    if (image->uri) {
        char resolved[512];
        resolve_path(model_path, image->uri, resolved, sizeof(resolved));
        JceTexture t = jce_texture_load_ex(pak, resolved, JCE_TEX_WRAP);
        if (jce_texture_valid(t)) return t;

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

        SDL_IOStream *io = SDL_IOFromFile(disk_path, "rb");
        if (io) {
            SDL_Surface *surf = IMG_Load_IO(io, true);
            if (surf) {
                if (surf->format != SDL_PIXELFORMAT_RGBA32) {
                    SDL_Surface *conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
                    SDL_DestroySurface(surf);
                    surf = conv;
                }
                if (surf) {
                    JceTexture tex = jce_texture_load_from_surface(surf, JCE_TEX_WRAP);
                    SDL_DestroySurface(surf);
                    return tex;
                }
            }
        }
        return JCE_TEXTURE_INVALID;
    }

    (void)data;
    return JCE_TEXTURE_INVALID;
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

static JcePbrMaterial *extract_materials(const JcePakArchive *pak,
                                          const char *model_path,
                                          cgltf_data *data,
                                          uint32_t *out_count)
{
    *out_count = 0;
    if (data->materials_count == 0) {
        /* Create a single default material. */
        JcePbrMaterial *mats = (JcePbrMaterial *)JCE_CALLOC(1, sizeof(JcePbrMaterial));
        if (!mats) return NULL;
        mats[0] = jce_pbr_material_default();
        *out_count = 1;
        return mats;
    }

    uint32_t count = (uint32_t)data->materials_count;
    JcePbrMaterial *mats = (JcePbrMaterial *)JCE_CALLOC(count, sizeof(JcePbrMaterial));
    if (!mats) return NULL;

    uint32_t i;
    for (i = 0; i < count; ++i) {
        const cgltf_material *src = &data->materials[i];
        JcePbrMaterial *dst = &mats[i];

        /* Start with defaults. */
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

            /* Albedo texture. */
            if (pbr->base_color_texture.texture &&
                pbr->base_color_texture.texture->image) {
                dst->albedo_map = load_gltf_texture(
                    pak, model_path, pbr->base_color_texture.texture->image, data);
            }

            /* Metallic-roughness texture. */
            if (pbr->metallic_roughness_texture.texture &&
                pbr->metallic_roughness_texture.texture->image) {
                dst->metallic_roughness_map = load_gltf_texture(
                    pak, model_path,
                    pbr->metallic_roughness_texture.texture->image, data);
            }
        }

        /* Normal map. */
        if (src->normal_texture.texture &&
            src->normal_texture.texture->image) {
            dst->normal_map = load_gltf_texture(
                pak, model_path, src->normal_texture.texture->image, data);
            dst->normal_scale = src->normal_texture.scale;
            if (dst->normal_scale == 0.0f) dst->normal_scale = 1.0f;
        }

        /* AO map. */
        if (src->occlusion_texture.texture &&
            src->occlusion_texture.texture->image) {
            dst->ao_map = load_gltf_texture(
                pak, model_path, src->occlusion_texture.texture->image, data);
            dst->ao_strength = src->occlusion_texture.scale;
            if (dst->ao_strength == 0.0f) dst->ao_strength = 1.0f;
        }

        /* Emissive. */
        if (src->emissive_texture.texture &&
            src->emissive_texture.texture->image) {
            dst->emissive_map = load_gltf_texture(
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

/* Build a mesh (static or skinned) from one glTF primitive. */
static void build_primitive(const cgltf_primitive *prim,
                             JceModelPrimitive *out,
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

        out->skinned_mesh = jce_skinned_mesh_create(verts, num_verts,
                                                     indices, num_indices);
        out->static_mesh = NULL;
        JCE_FREE(verts);
    }
    /* ---- Static PBR mesh (with tangent) ---- */
    else if (a_tan) {
        JcePbrVertex *verts = (JcePbrVertex *)JCE_CALLOC(
            num_verts, sizeof(JcePbrVertex));
        if (!verts) { JCE_FREE(indices); return; }

        uint32_t vi;
        for (vi = 0; vi < num_verts; ++vi) {
            cgltf_accessor_read_float(a_pos, vi, verts[vi].pos, 3);

            if (a_norm) cgltf_accessor_read_float(a_norm, vi, verts[vi].normal, 3);
            else { verts[vi].normal[0] = 0; verts[vi].normal[1] = 1; verts[vi].normal[2] = 0; }

            if (a_uv) cgltf_accessor_read_float(a_uv, vi, verts[vi].uv, 2);

            cgltf_accessor_read_float(a_tan, vi, verts[vi].tangent, 4);
        }

        out->skinned_mesh = jce_pbr_mesh_create(verts, num_verts,
                                                 indices, num_indices);
        out->static_mesh = NULL;
        JCE_FREE(verts);
    }
    /* ---- Static mesh (no tangent) — promote to PBR with default tangent ---- */
    else {
        JcePbrVertex *verts = (JcePbrVertex *)JCE_CALLOC(
            num_verts, sizeof(JcePbrVertex));
        if (!verts) { JCE_FREE(indices); return; }

        uint32_t vi;
        for (vi = 0; vi < num_verts; ++vi) {
            cgltf_accessor_read_float(a_pos, vi, verts[vi].pos, 3);

            if (a_norm) cgltf_accessor_read_float(a_norm, vi, verts[vi].normal, 3);
            else { verts[vi].normal[0] = 0; verts[vi].normal[1] = 1; verts[vi].normal[2] = 0; }

            if (a_uv) cgltf_accessor_read_float(a_uv, vi, verts[vi].uv, 2);

            /* Default tangent: +X, handedness +1. */
            verts[vi].tangent[0] = 1.0f;
            verts[vi].tangent[1] = 0.0f;
            verts[vi].tangent[2] = 0.0f;
            verts[vi].tangent[3] = 1.0f;
        }

        out->skinned_mesh = jce_pbr_mesh_create(verts, num_verts,
                                                  indices, num_indices);
        out->static_mesh = NULL;
        JCE_FREE(verts);
    }

    JCE_FREE(indices);
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

static JceModelNode *extract_nodes(cgltf_data *data,
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

    JceModelNode *nodes = (JceModelNode *)JCE_CALLOC(count, sizeof(JceModelNode));
    if (!nodes) { *out_count = 0; return NULL; }

    uint32_t idx = 0;
    for (ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node *gnode = &data->nodes[ni];
        if (!gnode->mesh) continue;

        JceModelNode *node = &nodes[idx];

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

        /* Primitives. */
        uint32_t num_prims = (uint32_t)gnode->mesh->primitives_count;
        node->num_primitives = num_prims;
        node->primitives = (JceModelPrimitive *)JCE_CALLOC(
            num_prims, sizeof(JceModelPrimitive));
        if (node->primitives) {
            uint32_t pi;
            for (pi = 0; pi < num_prims; ++pi) {
                build_primitive(&gnode->mesh->primitives[pi],
                                &node->primitives[pi], data);
            }
        }

        idx++;
    }

    *out_count = count;
    return nodes;
}

/* ================================================================== */
/* Main entry point                                                    */
/* ================================================================== */

JceModel *jce_gltf_load(const JcePakArchive *pak, const char *asset_path)
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
    cgltf_data *data = NULL;
    cgltf_result result = cgltf_parse(&options, buf, (cgltf_size)n, &data);
    if (result != cgltf_result_success) {
        LOG_ERROR(LOG_TAG, "cgltf_parse failed (%d): %s", (int)result, asset_path);
        JCE_FREE(buf);
        return NULL;
    }

    /* For GLB, binary data is inline; for glTF, this loads external buffers. */
    result = cgltf_load_buffers(&options, data, NULL);
    if (result != cgltf_result_success) {
        LOG_ERROR(LOG_TAG, "cgltf_load_buffers failed (%d): %s",
                  (int)result, asset_path);
        cgltf_free(data);
        JCE_FREE(buf);
        return NULL;
    }

    /* Safety: for GLB parsed from memory cgltf_load_buffers should have linked
       buffers[0].data to data->bin.  Ensure it is set even if the size check
       inside cgltf produced a silent mismatch so embedded textures can decode. */
    if (data->buffers_count > 0 && !data->buffers[0].data && data->bin) {
        data->buffers[0].data = (void *)data->bin;
        data->buffers[0].size = data->bin_size;
    }

    /* ---- Build model ---- */
    JceModel *model = (JceModel *)JCE_CALLOC(1, sizeof(JceModel));
    if (!model) {
        cgltf_free(data);
        JCE_FREE(buf);
        return NULL;
    }

    /* Materials. */
    model->materials = extract_materials(pak, asset_path, data,
                                          &model->num_materials);

    /* Nodes (meshes). */
    model->nodes = extract_nodes(data, &model->num_nodes);

    /* Skeleton. */
    model->skeleton = extract_skeleton(data);

    /* Animations. */
    model->anim_clips = extract_animations(data, &model->num_anims);

    LOG_DEBUG(LOG_TAG, "loaded %s: %u nodes, %u materials, %u anims%s",
              asset_path, model->num_nodes, model->num_materials,
              model->num_anims, model->skeleton ? " (skinned)" : "");

    cgltf_free(data);
    JCE_FREE(buf);
    return model;
}

/* ================================================================== */
/* Load from raw memory (no PAK required)                              */
/* ================================================================== */

JceModel *jce_gltf_load_memory(const void *file_data, uint32_t size,
                               const char *name)
{
    if (!file_data || size == 0) return NULL;
    const char *tag = name ? name : "<memory>";

    /* ---- Parse with cgltf ---- */
    cgltf_options options;
    memset(&options, 0, sizeof(options));
    cgltf_data *data = NULL;
    cgltf_result result = cgltf_parse(&options, file_data, (cgltf_size)size,
                                      &data);
    if (result != cgltf_result_success) {
        LOG_ERROR(LOG_TAG, "cgltf_parse failed (%d): %s", (int)result, tag);
        return NULL;
    }

    result = cgltf_load_buffers(&options, data, NULL);
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

    /* ---- Build model ---- */
    JceModel *model = (JceModel *)JCE_CALLOC(1, sizeof(JceModel));
    if (!model) {
        cgltf_free(data);
        return NULL;
    }

    /* Materials – no PAK so pass NULL; embedded textures still decoded. */
    model->materials = extract_materials(NULL, tag, data,
                                          &model->num_materials);
    model->nodes     = extract_nodes(data, &model->num_nodes);
    model->skeleton  = extract_skeleton(data);
    model->anim_clips = extract_animations(data, &model->num_anims);

    LOG_DEBUG(LOG_TAG, "loaded %s (memory): %u nodes, %u materials, %u anims%s",
              tag, model->num_nodes, model->num_materials,
              model->num_anims, model->skeleton ? " (skinned)" : "");

    cgltf_free(data);
    return model;
}
