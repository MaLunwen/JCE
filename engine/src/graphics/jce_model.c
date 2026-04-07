/*
 * jce_model.c  Composite model: accessors, draw, and destroy.
 *
 * The model struct is defined in jce_model_internal.h (shared with the
 * glTF loader which constructs JceModel instances).
 */

#include "jce_model_internal.h"
#include "jce_pbr_material.h"
#include <jce/graphics/jce_mesh.h>
#include <jce/graphics/jce_texture.h>
#include "jce_skinned_mesh.h"
#include "jce_skeleton.h"
#include "jce_animation.h"
#include <jce/graphics/jce_renderer.h>
#include "jce_renderer_internal.h"
#include "resource/jce_gltf_loader.h"
#include <jce/core/jce_log.h>

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>

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
        SDL_free(node->primitives);
    }
    SDL_free(model->nodes);

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
    SDL_free(model->materials);

    /* Free skeleton. */
    jce_skeleton_destroy(model->skeleton);

    /* Free animation clips. */
    for (uint32_t i = 0; i < model->num_anims; i++)
        jce_anim_clip_destroy(model->anim_clips[i]);
    SDL_free(model->anim_clips);

    SDL_free(model);
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

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];

        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];

            /* Bind material. */
            const JcePbrMaterial *mat = NULL;
            if (prim->material_index < model->num_materials)
                mat = &model->materials[prim->material_index];

            if (mat)
                jce_pbr_material_bind(mat, r, view_id);

            /* Determine which program to use. */
            JceShaderHandle prog_handle;

            if (prim->skinned_mesh) {
                /* Skinned path: upload bone palette and submit. */
                if (joint_matrices && num_joints > 0)
                    jce_skinned_mesh_set_bones(joint_matrices, num_joints);

                jce_skinned_mesh_submit(prim->skinned_mesh, r, view_id);
                prog_handle = jce_renderer_get_program_pbr_skinned(r);
            } else if (prim->static_mesh) {
                /* Static path: set world transform. */
                if (transform)
                    bgfx_set_transform(transform->m, 1);

                /* Set vertex/index buffers via mesh internals.
                 * We use jce_mesh_submit which also calls bgfx_submit,
                 * but we need PBR program. Use direct bgfx calls. */
                jce_skinned_mesh_submit(NULL, r, view_id);

                /* Actually for static meshes with PBR material,
                 * submit with PBR program directly. */
                prog_handle = jce_renderer_get_program_pbr(r);

                /* Re-do: set buffers + state + submit with PBR program. */
                if (transform)
                    bgfx_set_transform(transform->m, 1);
                jce_mesh_submit(prim->static_mesh, r, view_id);
                continue; /* mesh_submit already calls bgfx_submit */
            } else {
                continue;
            }

            /* Submit the draw call with the PBR program. */
            bgfx_program_handle_t bgfx_prog = { prog_handle.idx };
            bgfx_submit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);
        }
    }
}
