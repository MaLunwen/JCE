/*
 * jce_model.c  Composite model: accessors, draw, and destroy.
 *
 * The model struct is defined in jce_model_internal.h (shared with the
 * glTF loader which constructs JceModel instances).
 */

#include "jce_model_internal.h"
#include <jce/graphics/jce_pbr_material.h>
#include <jce/graphics/jce_mesh.h>
#include <jce/graphics/jce_texture.h>
#include <jce/animation/jce_skinned_mesh.h>
#include <jce/animation/jce_skeleton.h>
#include "animation/jce_animation.h"
#include <jce/graphics/jce_renderer.h>
#include "jce_renderer_internal.h"
#include "resource/jce_gltf_loader.h"
#include <jce/core/jce_log.h>

#include <bgfx/c99/bgfx.h>
#include "core/jce_memory.h"

#define LOG_TAG "jce_model"

/* ================================================================== */
/* Loading (delegates to glTF loader)                                  */
/* ================================================================== */

JceModel *jce_model_load_gltf(const PakArchive *pak, const char *asset_path)
{
    return jce_gltf_load(pak, asset_path);
}

/* ================================================================== */
/* Destroy                                                             */
/* ================================================================== */

void jce_model_destroy(JceModel *model)
{
    if (!model) return;

    /* Free meshes in all nodes. */
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            JceModelPrimitive *prim = &node->primitives[p];
            if (prim->static_mesh)
                jce_mesh_destroy(prim->static_mesh);
            if (prim->skinned_mesh)
                jce_skinned_mesh_destroy(prim->skinned_mesh);
        }
        JCE_FREE(node->primitives);
    }
    JCE_FREE(model->nodes);

    /* Free textures in materials. */
    for (uint32_t i = 0; i < model->num_materials; i++) {
        JcePbrMaterial *mat = &model->materials[i];
        if (jce_texture_valid(mat->albedo_map))
            jce_texture_destroy(mat->albedo_map);
        if (jce_texture_valid(mat->metallic_roughness_map))
            jce_texture_destroy(mat->metallic_roughness_map);
        if (jce_texture_valid(mat->normal_map))
            jce_texture_destroy(mat->normal_map);
        if (jce_texture_valid(mat->ao_map))
            jce_texture_destroy(mat->ao_map);
        if (jce_texture_valid(mat->emissive_map))
            jce_texture_destroy(mat->emissive_map);
    }
    JCE_FREE(model->materials);

    /* Free skeleton. */
    jce_skeleton_destroy(model->skeleton);

    /* Free animation clips. */
    for (uint32_t i = 0; i < model->num_anims; i++)
        jce_anim_clip_destroy(model->anim_clips[i]);
    JCE_FREE(model->anim_clips);

    JCE_FREE(model);
}

/* ================================================================== */
/* Accessors                                                           */
/* ================================================================== */

uint32_t jce_model_node_count(const JceModel *model)
{
    return model ? model->num_nodes : 0;
}

const JceModelNode *jce_model_get_node(const JceModel *model, uint32_t index)
{
    if (!model || index >= model->num_nodes) return NULL;
    return &model->nodes[index];
}

uint32_t jce_model_material_count(const JceModel *model)
{
    return model ? model->num_materials : 0;
}

const JcePbrMaterial *jce_model_get_material(const JceModel *model, uint32_t index)
{
    if (!model || index >= model->num_materials) return NULL;
    return &model->materials[index];
}

JceSkeleton *jce_model_get_skeleton(const JceModel *model)
{
    return model ? model->skeleton : NULL;
}

uint32_t jce_model_anim_count(const JceModel *model)
{
    return model ? model->num_anims : 0;
}

JceAnimClip *jce_model_get_anim(const JceModel *model, uint32_t index)
{
    if (!model || index >= model->num_anims) return NULL;
    return model->anim_clips[index];
}

/* ================================================================== */
/* Rendering                                                           */
/* ================================================================== */

void jce_model_draw(const JceModel *model,
                     const JceRenderer *r, uint16_t view_id,
                     const jce_mat4 *transform,
                     const jce_mat4 *joint_matrices,
                     uint32_t num_joints)
{
    if (!model || !r) return;

    jce_mat4 identity = jce_m4_identity();
    const jce_mat4 *root = transform ? transform : &identity;

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];

        /* node->local_transform is the pre-baked model-space world transform
           (cgltf_node_transform_world baked full hierarchy in the loader).
           Combine with the caller's model-to-world root transform. */
        jce_mat4 world = jce_m4_multiply(root, &node->local_transform);

        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];

            /* Bind PBR material. */
            const JcePbrMaterial *mat = NULL;
            if (prim->material_index < model->num_materials)
                mat = &model->materials[prim->material_index];
            if (mat)
                jce_pbr_material_bind(mat, r, view_id);

            if (prim->skinned_mesh) {
                JceShaderHandle prog_handle;

                if (jce_skinned_mesh_is_skinned(prim->skinned_mesh)) {
                    /* Fully skinned: bone matrices provide per-vertex transform.
                       Pre-multiply by root so the VS outputs world-space positions. */
                    if (joint_matrices && num_joints > 0) {
                        uint32_t nb = num_joints < 256 ? num_joints : 256;
                        jce_mat4 world_bones[256];
                        for (uint32_t bi = 0; bi < nb; bi++)
                            world_bones[bi] = jce_m4_multiply(root, &joint_matrices[bi]);
                        jce_skinned_mesh_set_bones(world_bones, nb);
                    } else if (model->skeleton) {
                        /* No live animation: evaluate rest/bind pose from skeleton. */
                        uint32_t num_j = jce_skeleton_joint_count(model->skeleton);
                        if (num_j > 256) num_j = 256;
                        jce_mat4 bind_pose[256];
                        jce_skeleton_evaluate(model->skeleton, NULL, bind_pose, num_j);
                        for (uint32_t bi = 0; bi < num_j; bi++)
                            bind_pose[bi] = jce_m4_multiply(root, &bind_pose[bi]);
                        jce_skinned_mesh_set_bones(bind_pose, num_j);
                    } else {
                        bgfx_set_transform(world.raw[0], 1);
                    }
                    prog_handle = jce_renderer_get_program_pbr_skinned(r);
                } else {
                    /* PBR static (has tangent, no joints): use node world. */
                    bgfx_set_transform(world.raw[0], 1);
                    prog_handle = jce_renderer_get_program_pbr(r);
                }

                jce_skinned_mesh_submit(prim->skinned_mesh, r, view_id);

                bgfx_program_handle_t bgfx_prog = { prog_handle.idx };
                bgfx_submit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);

            } else if (prim->static_mesh) {
                /* Basic mesh: apply transform, then submit. */
                jce_mat4 static_world;
                if (node->joint_parent_index >= 0 && model->skeleton) {
                    /* Static mesh directly parented to a skin joint (e.g. a held
                       weapon). Recover the joint's animated world transform:
                         joint_global = skin_matrix[i] × inverse(inv_bind[i])
                       Then: world = root × joint_global × node_local_from_joint */
                    uint32_t jidx = (uint32_t)node->joint_parent_index;
                    jce_mat4 inv_bind = jce_skeleton_get_inverse_bind(
                        model->skeleton, jidx);
                    jce_mat4 bind_mat = jce_m4_inverse(&inv_bind);
                    jce_mat4 joint_global;
                    if (joint_matrices && num_joints > jidx) {
                        joint_global = jce_m4_multiply(
                            &joint_matrices[jidx], &bind_mat);
                    } else {
                        /* No animation: bind pose global = bind_mat. */
                        joint_global = bind_mat;
                    }
                    jce_mat4 joint_world = jce_m4_multiply(root, &joint_global);
                    static_world = jce_m4_multiply(
                        &joint_world, &node->joint_local_matrix);
                } else {
                    static_world = world;
                }
                bgfx_set_transform(static_world.raw[0], 1);
                jce_mesh_submit(prim->static_mesh, r, view_id);
            }
        }
    }
}
