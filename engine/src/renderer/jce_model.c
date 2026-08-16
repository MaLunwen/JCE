/*
 * jce_model.c  Composite model: accessors, draw, and destroy.
 *
 * The model struct is defined in jce_model_internal.h (shared with the
 * glTF loader which constructs JceModel instances).
 */

#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/renderer/jce_skinned_mesh.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_skin_palette.h>
#include <jce/renderer/jce_texture.h>

#include "jce_model_internal.h"
#include "jce_renderer_internal.h"
#include "middleware/animation/jce_animation.h"
#include "os/core/jce_memory.h"
#include "jce_gltf_loader.h"

#include <bgfx/c99/bgfx.h>

#include <math.h>   /* fabsf / fmaxf for the editor checker-fallback encoding */
#include <string.h> /* memcpy / memcmp — MSVC pulls these in transitively, but
                     * Emscripten's strict clang needs the explicit include */

#define LOG_TAG "jce_model"

/* ── Pre-submit bind hook (Forward+ per-primitive cluster bind) ──────────
 * jce_model_draw issues one bgfx_submit per primitive, and bgfx clears the
 * per-draw texture-stage + uniform state between submits.  So a caller that
 * needs an extra per-submit bind (e.g. the scene renderer's Forward+ cluster
 * texture on stage 14, which fs_pbr_fwdplus reads) cannot bind once before
 * the call — every primitive after the first would lose it.  Instead the
 * caller arms this hook, which fires immediately BEFORE each color-pass
 * bgfx_submit inside jce_model_draw, so every primitive of a skinned/LOD
 * model gets the bind.  Render-thread only (same thread as jce_model_draw and
 * all material binds); no synchronization needed.  Default NULL => no-op =>
 * byte-identical to the unhooked path. */
static JceModelPreSubmitCb s_pre_submit_cb   = NULL;
static void               *s_pre_submit_user = NULL;

void jce_model_set_pre_submit_cb(JceModelPreSubmitCb cb, void *user)
{
    s_pre_submit_cb   = cb;
    s_pre_submit_user = user;
}

/* Optional per-draw material override.  When set, jce_model_draw binds THIS
 * material for every primitive instead of the model's embedded material — the
 * standard-engine behaviour where a renderer's assigned material slot overrides
 * the imported mesh's default materials.  The scene renderer arms it for a
 * MeshRenderer that authored a material/albedo texture (so a geometry-only
 * model converted to glTF still shows its scene-assigned atlas texture), and
 * clears it (NULL) immediately after the draw.  Render-thread only, mirroring
 * s_pre_submit_cb. */
static const JcePbrMaterial *s_material_override = NULL;

void jce_model_set_material_override(const JcePbrMaterial *mat)
{
    s_material_override = mat;
}

const JcePbrMaterial *jce_model_get_material_override(void)
{
    return s_material_override;
}

/* Editor "missing albedo → pink-black checker" fallback (TEXTURED/SHADED view
 * modes).  When armed, jce_model_draw renders any primitive whose effective
 * material has no valid albedo texture with the checker shader path — mirroring
 * the simple-mesh path so a glTF MODEL with no texture shows the same missing-
 * asset hint instead of a flat surface.  The fs_pbr.sc shader keys the checker
 * on a NEGATIVE normal_scale, so we just flip its sign on a local material
 * copy.  Scene renderer arms/clears it around the editor model draw; default
 * off => byte-identical to before. */
static bool s_albedo_checker = false;

void jce_model_set_albedo_checker(bool on)
{
    s_albedo_checker = on;
}

/* In-asset auto-LOD level for the NEXT model draw (large-world-opt P1 #6).
 * 0 = base geometry (LOD0, the legacy default).  A value >= 1 selects the
 * (level-1)'th reduced index buffer on each static primitive's mesh
 * (jce_skinned_mesh_submit_lod / _shadow_lod); primitives without that many
 * LODs fall through to their base index buffer.  Render-thread only (mirrors
 * s_material_override).  The scene renderer arms it from jce_lod_pick, draws,
 * and resets to 0 — so every draw that does NOT set it is byte-identical to
 * the pre-LOD path.  Skinned primitives ignore it (rigs keep full detail). */
static uint32_t s_draw_lod_level = 0;

void jce_model_set_draw_lod(uint32_t level)
{
    s_draw_lod_level = level;
}

static jce_mat4 compute_static_node_world(const JceModel *model,
                                           const JceModelNode *node,
                                           const jce_mat4 *root,
                                           const jce_mat4 *fallback_world,
                                           const jce_mat4 *joint_matrices,
                                           uint32_t num_joints)
{
    if (!fallback_world)
        return jce_m4_identity();
    if (!model || !node || !root)
        return *fallback_world;

    if (node->joint_parent_index >= 0 && model->skeleton) {
        uint32_t jidx = (uint32_t)node->joint_parent_index;
        jce_mat4 inv_bind = jce_skeleton_get_inverse_bind(model->skeleton, jidx);
        jce_mat4 bind_mat = jce_m4_inverse(&inv_bind);
        jce_mat4 joint_global;

        if (joint_matrices && num_joints > jidx)
            joint_global = jce_m4_multiply(&joint_matrices[jidx], &bind_mat);
        else
            joint_global = bind_mat;

        jce_mat4 joint_world = jce_m4_multiply(root, &joint_global);
        return jce_m4_multiply(&joint_world, &node->joint_local_matrix);
    }

    return *fallback_world;
}

/* ================================================================== */
/* Loading (delegates to glTF loader)                                  */
/* ================================================================== */

JceModel *jce_model_load_gltf(const JcePakArchive *pak, const char *asset_path)
{
    return jce_gltf_load(pak, asset_path);
}

JceModel *jce_model_load_gltf_memory(const void *data, uint32_t size,
                                      const char *name)
{
    return jce_gltf_load_memory(data, size, name);
}

bool jce_model_probe_rig_memory(const void *data, uint32_t size,
                                bool *out_has_skin, bool *out_has_anim)
{
    return jce_gltf_probe_rig_memory(data, size, out_has_skin, out_has_anim);
}

/* Worker-decode + render-thread-upload split (delegates to the glTF loader). */
JceModelCpu *jce_model_decode_gltf_cpu(const JcePakArchive *pak,
                                       const char *asset_path)
{
    return jce_gltf_decode_cpu(pak, asset_path);
}

JceModelCpu *jce_model_decode_gltf_cpu_memory(const void *data, uint32_t size,
                                              const char *name)
{
    return jce_gltf_decode_cpu_memory(data, size, name);
}

JceModel *jce_model_upload_gltf_cpu(JceModelCpu *cpu)
{
    return jce_gltf_upload_cpu(cpu);
}

void jce_model_gltf_cpu_free(JceModelCpu *cpu)
{
    jce_gltf_model_cpu_free(cpu);
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
            if (prim->morph)
                jce_morph_data_destroy(prim->morph);
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

    /* Free morph-weight tracks (FEATURE 3.1). */
    for (uint32_t i = 0; i < model->num_morph_anims; i++)
        jce_morph_weight_track_destroy(model->morph_anims[i].track);
    JCE_FREE(model->morph_anims);

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

bool jce_model_get_aabb(const JceModel *model, float out_min[3], float out_max[3])
{
    if (!model || !model->has_aabb || !out_min || !out_max) return false;
    out_min[0] = model->aabb_min[0]; out_min[1] = model->aabb_min[1]; out_min[2] = model->aabb_min[2];
    out_max[0] = model->aabb_max[0]; out_max[1] = model->aabb_max[1]; out_max[2] = model->aabb_max[2];
    return true;
}

/* Sum a single texture handle's resident bytes (RGBA8-equivalent w*h*4) using
 * the registry-reported resident size.  Invalid handles contribute 0. */
static uint64_t model_tex_bytes(JceTexture t)
{
    if (!jce_texture_valid(t)) return 0;
    uint32_t w = 0, h = 0;
    jce_texture_get_size(t, &w, &h);
    return (uint64_t)w * (uint64_t)h * 4u;
}

uint64_t jce_model_gpu_bytes(const JceModel *model)
{
    if (!model) return 0;

    /* Representative interleaved PBR vertex strides (pos/normal/tangent/uv,
     * plus joints+weights for skinned).  Exact strides aren't exposed for the
     * static path, so these are scale-correct estimates — the goal is a
     * residency number that tracks real VRAM, not an exact allocator query. */
    const uint64_t STATIC_STRIDE  = 32u;   /* pos(12)+nrm(8 oct? )+uv(8)+pad */
    const uint64_t SKINNED_STRIDE = 48u;   /* + joints(8)+weights(8) */
    const uint64_t INDEX_STRIDE   = 4u;    /* 32-bit indices (conservative) */

    uint64_t bytes = 0;

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            if (prim->static_mesh) {
                uint64_t vc = jce_mesh_vertex_count(prim->static_mesh);
                uint64_t ic = jce_mesh_index_count(prim->static_mesh);
                bytes += vc * STATIC_STRIDE + ic * INDEX_STRIDE;
            }
            if (prim->skinned_mesh) {
                uint64_t vc = jce_skinned_mesh_vertex_count(prim->skinned_mesh);
                uint64_t ic = jce_skinned_mesh_index_count(prim->skinned_mesh);
                bytes += vc * SKINNED_STRIDE + ic * INDEX_STRIDE;
            }
        }
    }

    for (uint32_t i = 0; i < model->num_materials; i++) {
        const JcePbrMaterial *m = &model->materials[i];
        bytes += model_tex_bytes(m->albedo_map);
        bytes += model_tex_bytes(m->metallic_roughness_map);
        bytes += model_tex_bytes(m->normal_map);
        bytes += model_tex_bytes(m->ao_map);
        bytes += model_tex_bytes(m->emissive_map);
    }

    return bytes;
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

const JceMorphData *jce_model_prim_morph(const JceModel *model,
                                         uint32_t node, uint32_t prim)
{
    if (!model || node >= model->num_nodes || !model->nodes) return NULL;
    const JceModelNode *nd = &model->nodes[node];
    if (prim >= nd->num_primitives || !nd->primitives) return NULL;
    return nd->primitives[prim].morph;
}

/* Narrow morph-walk accessors (FEATURE 3.1): let an external renderer enumerate
 * (node, prim) pairs and learn vertex counts without seeing the node/primitive
 * struct layouts.  Implementations mirror jce_model_prim_morph's bounds checks. */
uint32_t jce_model_node_prim_count(const JceModel *model, uint32_t node)
{
    if (!model || node >= model->num_nodes || !model->nodes) return 0;
    return model->nodes[node].num_primitives;
}

uint32_t jce_model_prim_vertex_count(const JceModel *model,
                                     uint32_t node, uint32_t prim)
{
    if (!model || node >= model->num_nodes || !model->nodes) return 0;
    const JceModelNode *nd = &model->nodes[node];
    if (prim >= nd->num_primitives || !nd->primitives) return 0;
    const JceModelPrimitive *p = &nd->primitives[prim];
    if (p->skinned_mesh)
        return jce_skinned_mesh_vertex_count(p->skinned_mesh);
    if (p->static_mesh)
        return jce_mesh_vertex_count(p->static_mesh);
    return 0;
}

const JceSkinnedMesh *jce_model_prim_skinned_mesh(const JceModel *model,
                                                  uint32_t node, uint32_t prim)
{
    if (!model || node >= model->num_nodes || !model->nodes) return NULL;
    const JceModelNode *nd = &model->nodes[node];
    if (prim >= nd->num_primitives || !nd->primitives) return NULL;
    return nd->primitives[prim].skinned_mesh;
}

uint32_t jce_model_morph_anim_count(const JceModel *model)
{
    return model ? model->num_morph_anims : 0;
}

const JceMorphWeightTrack *jce_model_morph_anim_track(
    const JceModel *model, uint32_t index,
    uint32_t *out_anim_index, uint32_t *out_node_index)
{
    if (!model || index >= model->num_morph_anims || !model->morph_anims)
        return NULL;
    const JceModelMorphAnim *ma = &model->morph_anims[index];
    if (out_anim_index) *out_anim_index = ma->anim_index;
    if (out_node_index) *out_node_index = ma->node_index;
    return ma->track;
}

/* ================================================================== */
/* Rendering                                                           */
/* ================================================================== */

void jce_model_draw_program(const JceModel *model,
                            const JceRenderer *r, uint16_t view_id,
                            const jce_mat4 *transform,
                            const jce_mat4 *joint_matrices,
                            uint32_t num_joints,
                            JceShaderHandle skinned_color_override)
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

            /* Bind PBR material.  A scene-armed override (MeshRenderer's
             * authored material/textures) wins over the model's embedded
             * material — standard "renderer material slot overrides mesh
             * default" behaviour. */
            const JcePbrMaterial *mat = NULL;
            if (prim->material_index < model->num_materials)
                mat = &model->materials[prim->material_index];
            if (s_material_override)
                mat = s_material_override;
            /* Editor checker fallback: no albedo texture → pink-black checker
             * (encoded as a negative normal_scale on a local copy). */
            JcePbrMaterial checker_mat;
            if (s_albedo_checker && mat && !jce_texture_valid(mat->albedo_map)) {
                checker_mat = *mat;
                checker_mat.normal_scale =
                    -fmaxf(fabsf(checker_mat.normal_scale), 0.0001f);
                mat = &checker_mat;
            }
            if (mat)
                jce_pbr_material_bind(mat, r, view_id);
            /* Honour the material's two-sided flag in the color submit (the
             * mesh submit hard-codes CULL_CW otherwise — see
             * jce_skinned_mesh_submit).  Cleared after the loop. */
            jce_skinned_mesh_set_submit_double_sided(mat && mat->double_sided);

            if (prim->skinned_mesh) {
                JceShaderHandle prog_handle;

                if (jce_skinned_mesh_is_skinned(prim->skinned_mesh)) {
                    /* Fully skinned: bone matrices provide per-vertex transform.
                       Pre-multiply by root so the VS outputs world-space positions. */
                    if (joint_matrices && num_joints > 0) {
                        jce_mat4 world_bones[256];
                        uint32_t nb = jce_skin_build_world_palette(
                            root, joint_matrices, num_joints, world_bones, 256);
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
                    /* Override only the truly-skinned color program (e.g. pbr_toon);
                     * INVALID sentinel => unchanged default => byte-identical. */
                    prog_handle = (skinned_color_override.idx != UINT16_MAX)
                                ? skinned_color_override
                                : jce_renderer_get_program_pbr_skinned(r);
                } else {
                    /* PBR static (has tangent, no joints): still respect
                       joint-parent attachment for props under bones. */
                    jce_mat4 static_world = compute_static_node_world(
                        model, node, root, &world, joint_matrices, num_joints);
                    bgfx_set_transform(static_world.raw[0], 1);
                    prog_handle = jce_renderer_get_program_pbr(r);
                }

                /* In-asset auto-LOD: a non-skinned (PBR static) primitive binds
                 * the armed reduced index set; skinned rigs keep full detail.
                 * Level 0 / no LOD => byte-identical to the base submit. */
                if (s_draw_lod_level > 0 &&
                    !jce_skinned_mesh_is_skinned(prim->skinned_mesh))
                    jce_skinned_mesh_submit_lod(prim->skinned_mesh, r, view_id,
                                                s_draw_lod_level - 1);
                else
                    jce_skinned_mesh_submit(prim->skinned_mesh, r, view_id);

                /* Per-submit bind hook (e.g. Forward+ cluster bind on stage
                 * 14): bgfx clears stage/uniform state between submits, so the
                 * hook must fire right before THIS submit, for every primitive.
                 * NULL => no-op => byte-identical to the unhooked path. */
                if (s_pre_submit_cb)
                    s_pre_submit_cb(s_pre_submit_user, view_id);

                bgfx_program_handle_t bgfx_prog = { prog_handle.idx };
                bgfx_submit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);

            } else if (prim->static_mesh) {
                /* Basic mesh: apply transform, then submit. */
                jce_mat4 static_world = compute_static_node_world(
                    model, node, root, &world, joint_matrices, num_joints);
                bgfx_set_transform(static_world.raw[0], 1);
                if (s_pre_submit_cb)
                    s_pre_submit_cb(s_pre_submit_user, view_id);
                jce_mesh_submit(prim->static_mesh, r, view_id);
            }
        }
    }
    /* Never leak the per-submit two-sided override into later draws. */
    jce_skinned_mesh_set_submit_double_sided(false);
}

void jce_model_draw(const JceModel *model,
                     const JceRenderer *r, uint16_t view_id,
                     const jce_mat4 *transform,
                     const jce_mat4 *joint_matrices,
                     uint32_t num_joints)
{
    /* Thin caller: INVALID override => byte-identical to the historical draw. */
    jce_model_draw_program(model, r, view_id, transform, joint_matrices,
                           num_joints, JCE_INVALID_SHADER);
}

/* Largest in-asset reduced-LOD count across the model's non-skinned primitives. */
uint32_t jce_model_max_lod(const JceModel *model)
{
    if (!model) return 0;
    uint32_t mx = 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceSkinnedMesh *sm = node->primitives[p].skinned_mesh;
            if (sm && !jce_skinned_mesh_is_skinned(sm)) {
                uint32_t c = jce_skinned_mesh_lod_count(sm);
                if (c > mx) mx = c;
            }
        }
    }
    return mx;
}

/* Index counts of the first drawable non-skinned primitive (for the inspector). */
uint32_t jce_model_lod_index_counts(const JceModel *model,
                                    uint32_t *out_base,
                                    uint32_t *out_lods,
                                    uint32_t max_levels)
{
    if (out_base) *out_base = 0;
    if (!model) return 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceSkinnedMesh *sm = node->primitives[p].skinned_mesh;
            if (!sm || jce_skinned_mesh_is_skinned(sm)) continue;
            if (out_base) *out_base = jce_skinned_mesh_index_count(sm);
            uint32_t lc = jce_skinned_mesh_lod_count(sm);
            uint32_t w = (lc < max_levels) ? lc : max_levels;
            for (uint32_t i = 0; i < w && out_lods; i++)
                out_lods[i] = jce_skinned_mesh_lod_index_count(sm, i);
            return w;
        }
    }
    return 0;
}

/* True when every primitive is non-skinned (static or PBR-static) so the model
 * can be GPU-instanced — a truly skinned primitive needs per-vertex bone
 * matrices that differ per instance and cannot share one instanced submit. */
bool jce_model_is_instanceable(const JceModel *model)
{
    if (!model) return false;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            if (prim->skinned_mesh && jce_skinned_mesh_is_skinned(prim->skinned_mesh))
                return false;
        }
    }
    return true;
}

/* True when EVERY drawable primitive of the model is a skinned mesh (and there
 * is at least one) — the model carries no static / non-skinned drawable prim.
 * The dual of jce_model_is_instanceable (all-static).  The bind-pose instancing
 * paths (color jce_model_draw_bindpose_instanced + shadow _bindpose_shadow_
 * instanced) draw ONLY skinned primitives via the instanced world-matrix
 * program, so they are correct only for a purely-skinned model; a mixed
 * skinned+static rig (body + static prop/eyes/accessory) must fall through to
 * the per-primitive path (jce_model_draw / _draw_shadow), which also draws and
 * casts its static sub-meshes.  Callers gate the bind-pose fast path on this. */
bool jce_model_is_purely_skinned(const JceModel *model)
{
    if (!model) return false;
    bool any_skinned = false;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            if (prim->skinned_mesh && jce_skinned_mesh_is_skinned(prim->skinned_mesh)) {
                any_skinned = true;
                continue;
            }
            /* Any drawable non-skinned primitive — a static mesh, or a
             * static-PBR primitive wrapped as a non-skinned skinned_mesh —
             * disqualifies the model from the skinned-only bind-pose path. */
            if (prim->static_mesh || prim->skinned_mesh)
                return false;
        }
    }
    return any_skinned;
}

/* GPU-instanced draw: render `count` copies of a NON-skinned model, one per
 * roots[i] model-to-world transform.  Each primitive is submitted ONCE with a
 * per-instance model-matrix buffer (vs_pbr_inst reads i_data0..3), so its PBR
 * material uniforms are written once per primitive instead of once per copy —
 * this is what keeps the per-frame uniform writes (bgfx's fixed VK scratch
 * buffer) bounded on dense streamed scenes.  Falls back to per-instance
 * jce_model_draw when the instanced program is unavailable or count==1. */
void jce_model_draw_instanced(const JceModel *model,
                              const JceRenderer *r, uint16_t view_id,
                              const jce_mat4 *roots, uint32_t count)
{
    jce_model_draw_instanced_tinted(model, r, view_id, roots, NULL, count);
}

/* Tint-aware sibling: each instance i additionally carries tints[i] (RGBA), fed
 * to the per-instance i_data4 attribute of vs_pbr_inst_tint and modulating
 * albedo in fs_pbr_tint exactly like a solo draw's u_baseColorFactor.  tints ==
 * NULL is the legacy no-tint path: stride-64 instance buffer + the plain
 * vs_pbr_inst program, byte-identical to before.  When tints != NULL but the
 * tint program is unavailable (older pak), it falls back to the no-tint program
 * (the per-entity colour is simply dropped — graceful degradation, not a crash).
 * (large-world-opt P1 #7) */
void jce_model_draw_instanced_tinted(const JceModel *model,
                                     const JceRenderer *r, uint16_t view_id,
                                     const jce_mat4 *roots,
                                     const jce_vec4 *tints, uint32_t count)
{
    if (!model || !r || !roots || count == 0) return;

    /* Tinted runs need the tint program (5-vec4 instance stride).  If it didn't
     * load, fall back to the plain instanced program with a 4-vec4 stride and no
     * tint (degrade gracefully rather than break batching). */
    JceShaderHandle prog_tint = jce_renderer_get_program_pbr_inst_tint(r);
    const bool use_tint = (tints != NULL) && (prog_tint.idx != UINT16_MAX);
    JceShaderHandle prog_inst = use_tint ? prog_tint
                                         : jce_renderer_get_program_pbr_inst(r);
    if (count == 1 || prog_inst.idx == UINT16_MAX) {
        /* Single instance / no instanced program: draw solo.  A tint is honoured
         * by binding it as the material's base-color factor on a copy of the
         * effective material so the solo result matches the instanced result. */
        for (uint32_t i = 0; i < count; i++) {
            const JcePbrMaterial *prev_ov = NULL;
            JcePbrMaterial tinted_ov;
            if (tints) {
                prev_ov = jce_model_get_material_override();
                /* Compose tint over the would-be effective material so a single
                 * tinted copy looks identical to a batched one. */
                tinted_ov = prev_ov ? *prev_ov : jce_pbr_material_default();
                tinted_ov.base_color_factor[0] = (prev_ov ? prev_ov->base_color_factor[0] : 1.0f) * tints[i].x;
                tinted_ov.base_color_factor[1] = (prev_ov ? prev_ov->base_color_factor[1] : 1.0f) * tints[i].y;
                tinted_ov.base_color_factor[2] = (prev_ov ? prev_ov->base_color_factor[2] : 1.0f) * tints[i].z;
                tinted_ov.base_color_factor[3] = (prev_ov ? prev_ov->base_color_factor[3] : 1.0f) * tints[i].w;
                jce_model_set_material_override(&tinted_ov);
            }
            jce_model_draw(model, r, view_id, &roots[i], NULL, 0);
            if (tints) jce_model_set_material_override(prev_ov);
        }
        return;
    }
    const bgfx_program_handle_t bgfx_prog = { (uint16_t)prog_inst.idx };
    /* 64 B (4 vec4 = mat4) for the legacy path; 80 B (mat4 + tint vec4) when the
     * per-instance tint stream is active. */
    const uint16_t stride = use_tint ? (uint16_t)(sizeof(jce_mat4) + sizeof(jce_vec4))
                                     : (uint16_t)sizeof(jce_mat4);

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        jce_mat4 node_lt = node->local_transform;

        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];

            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;  /* not instanceable */
            if (!sm && !st) continue;

            /* Material once per primitive (shared across all instances). */
            const JcePbrMaterial *mat = NULL;
            if (prim->material_index < model->num_materials)
                mat = &model->materials[prim->material_index];
            if (s_material_override)
                mat = s_material_override;
            JcePbrMaterial checker_mat;
            if (s_albedo_checker && mat && !jce_texture_valid(mat->albedo_map)) {
                checker_mat = *mat;
                checker_mat.normal_scale =
                    -fmaxf(fabsf(checker_mat.normal_scale), 0.0001f);
                mat = &checker_mat;
            }

            /* Batch through the transient instance buffer (handles the rare
             * case where a single group exceeds the remaining instance space). */
            uint32_t start = 0;
            while (start < count) {
                uint32_t want  = count - start;
                uint32_t avail = bgfx_get_avail_instance_data_buffer(want, stride);
                uint32_t nb    = want < avail ? want : avail;
                if (nb == 0) break;   /* no instance space left this frame */

                bgfx_instance_data_buffer_t idb;
                bgfx_alloc_instance_data_buffer(&idb, nb, stride);
                uint8_t *dst = (uint8_t *)idb.data;
                for (uint32_t i = 0; i < nb; i++) {
                    jce_mat4 world = jce_m4_multiply(&roots[start + i], &node_lt);
                    jce_mat4 sw = compute_static_node_world(model, node,
                                                            &roots[start + i],
                                                            &world, NULL, 0);
                    uint8_t *slot = dst + (size_t)i * stride;
                    memcpy(slot, sw.raw[0], sizeof(jce_mat4));
                    if (use_tint) {
                        /* Pack the tint immediately after the mat4 (i_data4). */
                        memcpy(slot + sizeof(jce_mat4), &tints[start + i],
                               sizeof(jce_vec4));
                    }
                }

                if (mat)
                    jce_pbr_material_bind(mat, r, view_id);
                const bool two_sided = (mat && mat->double_sided);
                jce_skinned_mesh_set_submit_double_sided(two_sided);

                if (sm) {
                    /* In-asset auto-LOD: bind the armed reduced index set so an
                     * instanced run of distant meshes draws fewer triangles
                     * while STILL sharing one instanced submit (the batch is
                     * keyed by (model, lod) upstream, so every instance in this
                     * run resolves to the same level).  Level 0 / no LOD =>
                     * identical to the base bind. */
                    if (s_draw_lod_level > 0)
                        jce_skinned_mesh_submit_lod(sm, r, view_id, s_draw_lod_level - 1);
                    else
                        jce_skinned_mesh_submit(sm, r, view_id);   /* binds VB/IB/state, no submit */
                } else {
                    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(st) };
                    bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(st) };
                    bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
                    if (ibh.idx != UINT16_MAX)
                        bgfx_set_index_buffer(ibh, 0, jce_mesh_index_count(st));
                    uint64_t st_state = BGFX_STATE_DEFAULT;
                    if (two_sided) st_state &= ~BGFX_STATE_CULL_MASK;
                    bgfx_set_state(st_state, 0);
                }

                bgfx_set_instance_data_buffer(&idb, 0, nb);
                if (s_pre_submit_cb)
                    s_pre_submit_cb(s_pre_submit_user, view_id);
                bgfx_submit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);

                start += nb;
            }
        }
    }
    jce_skinned_mesh_set_submit_double_sided(false);
}

/* 千万 S4 LOD-in-cull draw: the GPU cull (jce_gpu_scene_foliage_dispatch_lod)
 * has already compacted per-distance-band survivors into band_visible_vb[b] and
 * built band_indirect_buf[b] with band b's reduced-LOD index count.  Here we bind
 * the single drawable primitive's material once and issue ONE drawIndexedIndirect
 * per band at that band's reduced LOD index buffer (bands share one vertex buffer
 * — LODs differ only in the index set).  Band 0 = base LOD; band b>=1 = reduced
 * level (b-1).  Single-primitive foliage models only (the caller gates on
 * jce_model_gpu_drawable_count == 1); prog must be vs_pbr_inst + fs_pbr. */
void jce_model_draw_foliage_lod_indirect(const JceModel *model,
        const JceRenderer *r, uint16_t view_id, uint16_t prog_idx,
        uint32_t band_count, uint16_t visible_vb, uint16_t indirect_buf)
{
    if (!model || !r || band_count == 0u || prog_idx == UINT16_MAX ||
        visible_vb == UINT16_MAX || indirect_buf == UINT16_MAX) return;

    /* Locate drawable primitive 0 (same walk order as jce_model_gpu_primitive_*)
     * for its skinned mesh + material. */
    const JceModelPrimitive *prim0 = NULL;
    uint32_t drawable = 0;
    for (uint32_t n = 0; n < model->num_nodes && !prim0; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *psm = prim->skinned_mesh;
            JceMesh              *pst = prim->static_mesh;
            if (psm && jce_skinned_mesh_is_skinned(psm)) continue;
            if (!psm && !pst) continue;
            if (drawable == 0) { prim0 = prim; break; }
            drawable++;
        }
    }
    if (!prim0 || !prim0->skinned_mesh) return;   /* LOD carrier lives on skinned_mesh */
    const JceSkinnedMesh *sm = prim0->skinned_mesh;

    const JcePbrMaterial *mat = NULL;
    if (prim0->material_index < model->num_materials)
        mat = &model->materials[prim0->material_index];
    if (s_material_override) mat = s_material_override;

    const bgfx_program_handle_t prog = { prog_idx };
    const bool two_sided = (mat && mat->double_sided);
    uint64_t state = BGFX_STATE_DEFAULT;
    if (two_sided) state &= ~BGFX_STATE_CULL_MASK;
    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_skinned_mesh_get_vbh(sm) };
    const uint32_t lodc = jce_skinned_mesh_lod_count(sm);

    bgfx_dynamic_vertex_buffer_handle_t vis   = { visible_vb };
    bgfx_indirect_buffer_handle_t       indir = { indirect_buf };

    for (uint32_t b = 0; b < band_count; ++b) {
        /* Band b's reduced index buffer: band 0 = base; band k>=1 = reduced level
         * (k-1), clamped to the last available LOD so extra bands reuse it. */
        uint16_t ib_idx;
        if (b == 0u || lodc == 0u) {
            ib_idx = (uint16_t)jce_skinned_mesh_get_ibh(sm);
        } else {
            uint32_t lvl = (b - 1u < lodc) ? (b - 1u) : (lodc - 1u);
            ib_idx = (uint16_t)jce_skinned_mesh_lod_ibh(sm, lvl);
        }
        bgfx_index_buffer_handle_t ibh = { ib_idx };

        /* BGFX_DISCARD_ALL clears bindings after each submit → (re)bind per band.
         * The instance stream is the WHOLE band-partitioned survivor buffer; the
         * indirect element's startInstance (= band * cap_band, written by
         * cs_foliage_indirect) selects the band's partition. */
        if (mat) jce_pbr_material_bind(mat, r, view_id);
        bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
        if (ib_idx != UINT16_MAX)
            bgfx_set_index_buffer(ibh, 0, UINT32_MAX);
        bgfx_set_instance_data_from_dynamic_vertex_buffer(vis, 0, UINT32_MAX);
        bgfx_set_state(state, 0);
        if (s_pre_submit_cb)
            s_pre_submit_cb(s_pre_submit_user, view_id);
        bgfx_submit_indirect(view_id, prog, indir, (uint16_t)b, 1, 0,
                             BGFX_DISCARD_ALL);
    }
}

/* Nanite-lite V2 draw: ONE bgfx_submit_indirect over the meshlet-grouped
 * index buffer — cs_meshlet_cull has already written one drawIndexedIndirect
 * element per meshlet (culled clusters = zero-index degenerates), so the GPU
 * rasterises only the surviving clusters of this hero mesh.  Binds prim 0's
 * material + the entity world transform (applies to every element of the one
 * submit).  prog = the SOLO pbr program (vs_pbr reads the transform cache).
 * Single-drawable models only; global PBR state must already be bound. */
void jce_model_draw_meshlet_culled(const JceModel *model, const JceRenderer *r,
                                   uint16_t view_id, uint16_t prog_idx,
                                   const jce_mat4 *world,
                                   uint16_t indirect_buf, uint32_t count)
{
    if (!model || !r || !world || prog_idx == UINT16_MAX ||
        indirect_buf == UINT16_MAX || count == 0u) return;

    const JceModelPrimitive *prim0 = NULL;
    for (uint32_t n = 0; n < model->num_nodes && !prim0; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *psm = prim->skinned_mesh;
            if (!psm || jce_skinned_mesh_is_skinned(psm)) continue;
            prim0 = prim; break;
        }
    }
    if (!prim0) return;
    const JceSkinnedMesh *sm = prim0->skinned_mesh;
    uint16_t ml_ib = (uint16_t)jce_skinned_mesh_meshlet_ibh(sm);
    if (ml_ib == UINT16_MAX) return;

    const JcePbrMaterial *mat = NULL;
    if (prim0->material_index < model->num_materials)
        mat = &model->materials[prim0->material_index];
    if (s_material_override) mat = s_material_override;

    bgfx_program_handle_t       prog = { prog_idx };
    bgfx_vertex_buffer_handle_t vbh  = { (uint16_t)jce_skinned_mesh_get_vbh(sm) };
    bgfx_index_buffer_handle_t  ibh  = { ml_ib };
    bgfx_indirect_buffer_handle_t ind = { indirect_buf };
    uint64_t state = BGFX_STATE_DEFAULT;
    if (mat && mat->double_sided) state &= ~BGFX_STATE_CULL_MASK;

    if (mat) jce_pbr_material_bind(mat, r, view_id);
    bgfx_set_transform(world->raw[0], 1);
    bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
    bgfx_set_index_buffer(ibh, 0, UINT32_MAX);
    bgfx_set_state(state, 0);
    if (s_pre_submit_cb)
        s_pre_submit_cb(s_pre_submit_user, view_id);
    /* _num is uint32 — no cast: a V3 DAG roughly doubles cluster counts and
     * a >65535-cluster hero would truncate into a vanished-at-distance mesh. */
    bgfx_submit_indirect(view_id, prog, ind, 0, count, 0,
                         BGFX_DISCARD_ALL);
}

/* V4: depth-only meshlet-culled draw into a SHADOW cascade view.  Same
 * per-cluster indirect args as the color path (produced by a shadow-mode
 * cull dispatch against the light frustum), but binds the static depth-only
 * shadow program, no material, and the shadow depth state — so a dense hero
 * rasterises only its light-frustum-visible clusters into each cascade. */
void jce_model_draw_meshlet_culled_shadow(const JceModel *model,
                                          const JceRenderer *r,
                                          uint16_t view_id,
                                          const jce_mat4 *world,
                                          uint16_t indirect_buf, uint32_t count)
{
    if (!model || !r || !world || indirect_buf == UINT16_MAX || count == 0u)
        return;
    JceShaderHandle sh = jce_renderer_get_program_shadow(r);
    if (sh.idx == UINT16_MAX) return;

    const JceModelPrimitive *prim0 = NULL;
    for (uint32_t n = 0; n < model->num_nodes && !prim0; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceSkinnedMesh *psm = node->primitives[p].skinned_mesh;
            if (!psm || jce_skinned_mesh_is_skinned(psm)) continue;
            prim0 = &node->primitives[p]; break;
        }
    }
    if (!prim0) return;
    const JceSkinnedMesh *sm = prim0->skinned_mesh;
    uint16_t ml_ib = (uint16_t)jce_skinned_mesh_meshlet_ibh(sm);
    if (ml_ib == UINT16_MAX) return;

    bgfx_program_handle_t         prog = { (uint16_t)sh.idx };
    bgfx_vertex_buffer_handle_t   vbh  = { (uint16_t)jce_skinned_mesh_get_vbh(sm) };
    bgfx_index_buffer_handle_t    ibh  = { ml_ib };
    bgfx_indirect_buffer_handle_t ind  = { indirect_buf };
    uint64_t state = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                   | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA;

    bgfx_set_transform(world->raw[0], 1);
    bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
    bgfx_set_index_buffer(ibh, 0, UINT32_MAX);
    bgfx_set_state(state, 0);
    bgfx_submit_indirect(view_id, prog, ind, 0, count, 0, BGFX_DISCARD_ALL);
}

/* Scatter-shadow instanced submit (千万 ③): ONE depth-only instanced draw of
 * the model's single drawable primitive at a REDUCED in-asset LOD level, with
 * the per-instance world matrices sourced from a persistent dynamic VB (the
 * scatter's COMPUTE_READ roots buffer doubles as plain instance data — zero
 * CPU copies).  level clamps to the coarsest available LOD; UINT32_MAX also
 * means "coarsest".  Depth-only state; no material/Forward+ binding. */
void jce_model_draw_shadow_instanced_lod(const JceModel *model,
                                         const JceRenderer *r,
                                         uint16_t view_id, uint16_t program_idx,
                                         uint16_t inst_vb, uint32_t count,
                                         uint32_t level)
{
    if (!model || !r || count == 0
        || inst_vb == UINT16_MAX || program_idx == UINT16_MAX) return;

    /* Locate drawable primitive 0 (same walk as jce_model_gpu_primitive_*). */
    const JceModelPrimitive *prim0 = NULL;
    for (uint32_t n = 0; n < model->num_nodes && !prim0; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *psm = prim->skinned_mesh;
            JceMesh              *pst = prim->static_mesh;
            if (psm && jce_skinned_mesh_is_skinned(psm)) continue;
            if (!psm && !pst) continue;
            prim0 = prim; break;
        }
    }
    if (!prim0) return;

    uint16_t vb_idx, ib_idx;
    uint32_t nidx;
    if (prim0->skinned_mesh) {
        const JceSkinnedMesh *sm = prim0->skinned_mesh;
        uint32_t lodc = jce_skinned_mesh_lod_count(sm);
        uint32_t lvl  = (lodc == 0u) ? UINT32_MAX
                       : (level >= lodc ? lodc - 1u : level);
        vb_idx = (uint16_t)jce_skinned_mesh_get_vbh(sm);
        if (lvl == UINT32_MAX) {
            ib_idx = (uint16_t)jce_skinned_mesh_get_ibh(sm);
            nidx   = jce_skinned_mesh_index_count(sm);
        } else {
            ib_idx = (uint16_t)jce_skinned_mesh_lod_ibh(sm, lvl);
            nidx   = jce_skinned_mesh_lod_index_count(sm, lvl);
        }
    } else {
        JceMesh *st = prim0->static_mesh;
        vb_idx = (uint16_t)jce_mesh_get_vbh(st);
        ib_idx = (uint16_t)jce_mesh_get_ibh(st);
        nidx   = jce_mesh_index_count(st);
    }
    if (vb_idx == UINT16_MAX) return;

    bgfx_vertex_buffer_handle_t         vbh  = { vb_idx };
    bgfx_index_buffer_handle_t          ibh  = { ib_idx };
    bgfx_dynamic_vertex_buffer_handle_t inst = { inst_vb };
    bgfx_program_handle_t               prog = { program_idx };
    bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
    if (ib_idx != UINT16_MAX)
        bgfx_set_index_buffer(ibh, 0, nidx);
    bgfx_set_instance_data_from_dynamic_vertex_buffer(inst, 0, count);
    bgfx_set_state(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                 | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA, 0);
    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

/* GPU crowd instancing: draw N uniquely-posed instances of a SKINNED model in
 * one instanced submit per skinned primitive.  Unlike jce_model_draw_instanced_
 * tinted (which skips actually-skinned meshes), the per-instance stream here is
 * NOT a world matrix — it is a single float, i_data0.x = the instance's bone
 * PALETTE BASE (in bones) into the shared bone texture (bound at slot 9).  The
 * palette is world-space, so no node/world transform is applied here (it is
 * already folded into each bone matrix, exactly as the per-character skinned
 * path relies on).  prog must be vs_pbr_skinned_inst + fs_pbr.  Handles passed
 * as raw idxs to keep bgfx out of the public header. */
void jce_model_draw_crowd_instanced(const JceModel *model, const JceRenderer *r,
                                    uint16_t view_id, uint16_t prog_idx,
                                    const jce_mat4 *worlds, const uint32_t *bases,
                                    uint32_t count,
                                    uint16_t s_bones_idx, uint16_t bone_tex_idx,
                                    uint16_t u_params_idx,
                                    float tex_w, float tex_h)
{
    if (!model || !r || !worlds || !bases || count == 0 || prog_idx == UINT16_MAX) return;

    const bgfx_program_handle_t prog     = { prog_idx };
    const bgfx_uniform_handle_t s_bones  = { s_bones_idx };
    const bgfx_texture_handle_t bone_tex = { bone_tex_idx };
    const bgfx_uniform_handle_t u_params = { u_params_idx };
    /* 80 B/instance: world mat4 (i_data0..3) + palette base (i_data4.x). */
    const uint16_t stride = (uint16_t)(sizeof(jce_mat4) + sizeof(jce_vec4));
    float params[4] = { tex_w, tex_h, 0.0f, 0.0f };

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            if (!sm || !jce_skinned_mesh_is_skinned(sm)) continue;  /* skinned only */

            const JcePbrMaterial *mat = NULL;
            if (prim->material_index < model->num_materials)
                mat = &model->materials[prim->material_index];
            if (s_material_override)
                mat = s_material_override;
            const bool two_sided = (mat && mat->double_sided);

            uint32_t start = 0;
            while (start < count) {
                uint32_t want  = count - start;
                uint32_t avail = bgfx_get_avail_instance_data_buffer(want, stride);
                uint32_t nb    = want < avail ? want : avail;
                if (nb == 0) break;   /* no instance space left this frame */

                bgfx_instance_data_buffer_t idb;
                bgfx_alloc_instance_data_buffer(&idb, nb, stride);
                uint8_t *dst = (uint8_t *)idb.data;
                for (uint32_t i = 0; i < nb; i++) {
                    uint8_t *slot = dst + (size_t)i * stride;
                    memcpy(slot, worlds[start + i].raw[0], sizeof(jce_mat4)); /* i_data0..3 */
                    float base4[4] = { (float)bases[start + i], 0.0f, 0.0f, 0.0f };
                    memcpy(slot + sizeof(jce_mat4), base4, sizeof(base4));     /* i_data4 */
                }

                if (mat)
                    jce_pbr_material_bind(mat, r, view_id);
                jce_skinned_mesh_set_submit_double_sided(two_sided);
                jce_skinned_mesh_submit(sm, r, view_id);   /* binds VB/IB/state */
                /* Stage 4 ALIASES s_emissive (all 16 stages taken; 9-12 are the
                 * CSM cascades, which the pre-submit hook re-binds after us —
                 * the original slot-9 bind was silently stomped by cascade 0).
                 * Must land AFTER jce_pbr_material_bind (which put the material
                 * emissive/white at 4); harmless to shading because the crowd
                 * gate excludes emissive materials (fs: emissiveTex * factor,
                 * factor == 0). */
                bgfx_set_texture(4, s_bones, bone_tex, UINT32_MAX);
                bgfx_set_uniform(u_params, params, 1);
                bgfx_set_instance_data_buffer(&idb, 0, nb);
                if (s_pre_submit_cb)
                    s_pre_submit_cb(s_pre_submit_user, view_id);
                bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);

                start += nb;
            }
        }
    }
    jce_skinned_mesh_set_submit_double_sided(false);
}

/* GPU bind-pose instancing: draw `count` instances of a SKINNED model that are
 * NOT animating (no active pose — beyond the SR_ANIM_INSTANCE_MAX animation cap,
 * or an idle animator) in one instanced submit per skinned primitive.  A
 * bind-pose skinned mesh has its vertices in MODEL space, so it is effectively a
 * static mesh: the plain instanced PBR program (vs_pbr_inst) transforms it by
 * the per-instance world matrix (i_data0..3) — no bone palette / texture read
 * needed.  This collapses the large bind-pose majority of a big crowd (the real
 * draw wall once the anim cap is hit) with the already-proven world-matrix path. */
void jce_model_draw_bindpose_instanced(const JceModel *model, const JceRenderer *r,
                                       uint16_t view_id,
                                       const jce_mat4 *worlds, uint32_t count)
{
    if (!model || !r || !worlds || count == 0) return;
    JceShaderHandle prog_inst = jce_renderer_get_program_pbr_inst(r);
    if (prog_inst.idx == UINT16_MAX) return;
    const bgfx_program_handle_t prog = { (uint16_t)prog_inst.idx };
    const uint16_t stride = (uint16_t)sizeof(jce_mat4);   /* 64 B: world mat4 (i_data0..3) */

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            if (!sm || !jce_skinned_mesh_is_skinned(sm)) continue;  /* skinned only */

            const JcePbrMaterial *mat = NULL;
            if (prim->material_index < model->num_materials)
                mat = &model->materials[prim->material_index];
            if (s_material_override)
                mat = s_material_override;
            const bool two_sided = (mat && mat->double_sided);

            uint32_t start = 0;
            while (start < count) {
                uint32_t want  = count - start;
                uint32_t avail = bgfx_get_avail_instance_data_buffer(want, stride);
                uint32_t nb    = want < avail ? want : avail;
                if (nb == 0) break;

                bgfx_instance_data_buffer_t idb;
                bgfx_alloc_instance_data_buffer(&idb, nb, stride);
                uint8_t *dst = (uint8_t *)idb.data;
                for (uint32_t i = 0; i < nb; i++)
                    memcpy(dst + (size_t)i * stride, worlds[start + i].raw[0], sizeof(jce_mat4));

                if (mat)
                    jce_pbr_material_bind(mat, r, view_id);
                jce_skinned_mesh_set_submit_double_sided(two_sided);
                jce_skinned_mesh_submit(sm, r, view_id);   /* binds skinned VB/IB/state */
                bgfx_set_instance_data_buffer(&idb, 0, nb);
                if (s_pre_submit_cb)
                    s_pre_submit_cb(s_pre_submit_user, view_id);
                bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);

                start += nb;
            }
        }
    }
    jce_skinned_mesh_set_submit_double_sided(false);
}

/* Instances per transient-buffer chunk; see the overflow note below. */
#define JCE_INST_CHUNK_MAX 8192u

/* Gathered variant: fills the instance buffer straight from the caller's
 * records through an index list, instead of making the caller pack world+tint
 * into two scratch arrays that this function immediately unpacks again.
 *
 * That round trip was 22% of the frame at the 200k bench -- the gather loop and
 * this function's copy loop are separately the two hottest lines in it -- and
 * it moves 160 bytes per instance where 80 will do. `src_stride`/`src_offset`
 * describe where the contiguous world+tint pair lives inside one record;
 * `ord[k]` selects the record. NULL ord means identity.
 *
 * Internal on purpose: jce_mesh_draw_instanced_tinted stays the public JCE_API
 * shape and now forwards to this. */
void jce_mesh_draw_instanced_gathered(const JceMesh *mesh, const JceRenderer *r,
                                      uint16_t view_id, const void *src,
                                      size_t src_stride, size_t src_offset,
                                      const uint32_t *ord, uint32_t count,
                                      bool has_tint, uint64_t state,
                                      void (*pre_submit)(void *user, uint16_t view_id),
                                      void *pre_submit_user);

void jce_mesh_draw_instanced_tinted(const JceMesh *mesh, const JceRenderer *r,
                                    uint16_t view_id, const jce_mat4 *worlds,
                                    const jce_vec4 *tints, uint32_t count,
                                    uint64_t state,
                                    void (*pre_submit)(void *user, uint16_t view_id),
                                    void *pre_submit_user)
{
    if (!mesh || !r || !worlds || count == 0) return;

    /* Tint stream needs the 5-vec4 (80 B) tint program; fall back to the plain
     * 4-vec4 (64 B) instanced program (tint dropped) if it did not load. */
    JceShaderHandle prog_tint = jce_renderer_get_program_pbr_inst_tint(r);
    const bool use_tint = (tints != NULL) && (prog_tint.idx != UINT16_MAX);
    JceShaderHandle prog = use_tint ? prog_tint
                                    : jce_renderer_get_program_pbr_inst(r);
    if (prog.idx == UINT16_MAX) return;   /* no instanced program → caller keeps solo */
    const bgfx_program_handle_t bgfx_prog = { (uint16_t)prog.idx };
    const uint16_t stride = use_tint ? (uint16_t)(sizeof(jce_mat4) + sizeof(jce_vec4))
                                     : (uint16_t)sizeof(jce_mat4);

    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(mesh) };
    bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(mesh) };
    const uint64_t st_state = state ? state : BGFX_STATE_DEFAULT;

    uint32_t start = 0;
    while (start < count) {
        /* Chunk cap, because neither of bgfx's two numbers can be trusted at
         * scale here. Asking for 200,000 x 80 B in one go, bgfx answered
         * avail = 53,687,090 -- about 4 GiB, when the engine configured
         * limits.maxTransientVbSize = 32 MiB -- and then handed back a buffer
         * reporting size = 4,294,967,200, which is 2^32 - 96: its own 32-bit
         * size computation had overflowed. Writing the 16 MB payload into that
         * ran off the end of the real pool, a 100%-reproducible
         * ACCESS_VIOLATION (WRITE, faulting exactly on a page boundary).
         *
         * A size check does not catch it -- 16,000,000 < 4,294,967,200 passes.
         * So bound the ask instead: JCE_INST_CHUNK_MAX slots is 640 KB per
         * chunk, small enough that bgfx's accounting stays in range, and the
         * loop was already written to iterate. */
        uint32_t want  = count - start;
        if (want > JCE_INST_CHUNK_MAX) want = JCE_INST_CHUNK_MAX;
        uint32_t avail = bgfx_get_avail_instance_data_buffer(want, stride);
        uint32_t nb    = want < avail ? want : avail;
        if (nb == 0) break;   /* no instance space left this frame */

        bgfx_instance_data_buffer_t idb;
        memset(&idb, 0, sizeof idb);
        bgfx_alloc_instance_data_buffer(&idb, nb, stride);

        /* RE-READ idb.num. bgfx may hand back fewer instances than asked for,
         * and the count it actually gave is the only bound the fill loop may
         * use -- writing `nb` slots into a buffer sized for idb.num runs off
         * the end of the transient pool. That was a 100%-reproducible
         * ACCESS_VIOLATION here at 200k (WRITE, faulting on a page boundary),
         * and it is not a new hazard: apply_transient_limits in
         * jce_renderer.c already documents that "every consumer degrades
         * cleanly today (rq re-reads idb.num after alloc; ImGui clamps)".
         * This consumer did not. */
        /* 64-bit compare: nb*stride in 32 bits is exactly how the overflow
         * above slipped through. */
        if (!idb.data || idb.num == 0 ||
            (uint64_t)idb.size < (uint64_t)nb * (uint64_t)stride) {
            static uint32_t s_bad = 0;
            if ((s_bad++ % 120u) == 0u)
                LOG_WARN(LOG_TAG, "instance buffer unusable: asked %u x %u B, "
                         "got data=%p size=%u num=%u -- dropping the rest",
                         nb, (unsigned)stride, (void *)idb.data, idb.size,
                         idb.num);
            break;
        }
        if (idb.num < nb) {
            static uint32_t s_warned = 0;
            if ((s_warned++ % 120u) == 0u)
                LOG_WARN(LOG_TAG,
                         "instance pool short: asked %u x %u B (avail said %u), "
                         "got %u -- drawing %u this chunk",
                         nb, (unsigned)stride, avail, idb.num, idb.num);
            nb = idb.num;
        }
        uint8_t *dst = (uint8_t *)idb.data;
        for (uint32_t i = 0; i < nb; i++) {
            uint8_t *slot = dst + (size_t)i * stride;
            memcpy(slot, worlds[start + i].raw[0], sizeof(jce_mat4));
            if (use_tint)
                memcpy(slot + sizeof(jce_mat4), &tints[start + i], sizeof(jce_vec4));
        }

        bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
        if (ibh.idx != UINT16_MAX)
            bgfx_set_index_buffer(ibh, 0, jce_mesh_index_count(mesh));
        bgfx_set_state(st_state, 0);
        bgfx_set_instance_data_buffer(&idb, 0, nb);
        /* Bind the shared material + frame-global state LAST (per chunk) so the
         * uniform/texture updates land in THIS submit's replay range — same
         * discipline as the model path's pre-submit hook. */
        if (pre_submit) pre_submit(pre_submit_user, view_id);
        bgfx_submit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);

        start += nb;
    }
}

void jce_mesh_draw_instanced_gathered(const JceMesh *mesh, const JceRenderer *r,
                                      uint16_t view_id, const void *src,
                                      size_t src_stride, size_t src_offset,
                                      const uint32_t *ord, uint32_t count,
                                      bool has_tint, uint64_t state,
                                      void (*pre_submit)(void *user, uint16_t view_id),
                                      void *pre_submit_user)
{
    if (!mesh || !r || !src || count == 0) return;

    JceShaderHandle prog_tint = jce_renderer_get_program_pbr_inst_tint(r);
    const bool use_tint = has_tint && (prog_tint.idx != UINT16_MAX);
    JceShaderHandle prog = use_tint ? prog_tint
                                    : jce_renderer_get_program_pbr_inst(r);
    if (prog.idx == UINT16_MAX) return;
    const bgfx_program_handle_t bgfx_prog = { (uint16_t)prog.idx };
    const uint16_t stride = use_tint ? (uint16_t)(sizeof(jce_mat4) + sizeof(jce_vec4))
                                     : (uint16_t)sizeof(jce_mat4);

    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(mesh) };
    bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(mesh) };
    const uint64_t st_state = state ? state : BGFX_STATE_DEFAULT;
    const uint8_t *base = (const uint8_t *)src + src_offset;

    uint32_t start = 0;
    while (start < count) {
        uint32_t want = count - start;
        if (want > JCE_INST_CHUNK_MAX) want = JCE_INST_CHUNK_MAX;
        uint32_t avail = bgfx_get_avail_instance_data_buffer(want, stride);
        uint32_t nb    = want < avail ? want : avail;
        if (nb == 0) break;

        bgfx_instance_data_buffer_t idb;
        memset(&idb, 0, sizeof idb);
        bgfx_alloc_instance_data_buffer(&idb, nb, stride);
        if (!idb.data || idb.num == 0 ||
            (uint64_t)idb.size < (uint64_t)nb * (uint64_t)stride) break;
        if (idb.num < nb) nb = idb.num;

        uint8_t *dst = (uint8_t *)idb.data;
        /* Constant copy sizes, and the branch hoisted out of the loop.
         *
         * memcpy with a VARIABLE length is a real call into the CRT, and at
         * 194k instances a frame that showed up as 15.5% of the frame sitting
         * on two adjacent instructions inside VCRUNTIME140. With the size a
         * compile-time constant the compiler inlines it to a few vector moves,
         * which is what the two struct assignments this replaced were doing.
         * jce_mat4/jce_vec4 carry no alignment attribute, so these are
         * unaligned moves and idb.data needs no particular alignment. */
        if (use_tint) {
            enum { JCE_INST_TINTED_BYTES = sizeof(jce_mat4) + sizeof(jce_vec4) };
            for (uint32_t i = 0; i < nb; i++) {
                const uint32_t k = ord ? ord[start + i] : (start + i);
                memcpy(dst + (size_t)i * JCE_INST_TINTED_BYTES,
                       base + (size_t)k * src_stride, JCE_INST_TINTED_BYTES);
            }
        } else {
            for (uint32_t i = 0; i < nb; i++) {
                const uint32_t k = ord ? ord[start + i] : (start + i);
                memcpy(dst + (size_t)i * sizeof(jce_mat4),
                       base + (size_t)k * src_stride, sizeof(jce_mat4));
            }
        }

        bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
        if (ibh.idx != UINT16_MAX)
            bgfx_set_index_buffer(ibh, 0, jce_mesh_index_count(mesh));
        bgfx_set_state(st_state, 0);
        bgfx_set_instance_data_buffer(&idb, 0, nb);
        if (pre_submit) pre_submit(pre_submit_user, view_id);
        bgfx_submit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);
        start += nb;
    }
}


/* ── GPU-driven instancing (roadmap #18, Phase 0+1) ──────────────────────
 *
 * The GPU-driven color path stores ONE world matrix per resident instance in a
 * persistent GPU buffer and culls/compacts on the GPU.  To keep the per-record
 * matrix unambiguous, the GPU path only handles models with exactly ONE
 * drawable static primitive (the dominant instancing case — repeated single-
 * mesh props); multi-primitive / multi-node models fall back to the CPU
 * jce_model_draw_instanced path (each primitive there folds its own node-local
 * transform per instance, which a single shared instance buffer cannot express).
 *
 * jce_model_gpu_instanceable returns true for that single-primitive case and
 * writes the primitive's pre-baked node-local (model-space world) transform to
 * *out_node_lt, so the caller folds it into each instance's world matrix
 * (record.world = entity_world * node_lt) before uploading to the GPUScene. */
bool jce_model_gpu_instanceable(const JceModel *model, jce_mat4 *out_node_lt)
{
    if (!model) return false;
    const JceModelNode *the_node = NULL;
    const JceModelPrimitive *the_prim = NULL;
    uint32_t drawable = 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) return false; /* skinned */
            if (!sm && !st) continue;   /* nothing to draw */
            /* A joint-parented static prop needs compute_static_node_world
             * per-instance — the shared instance buffer can't carry it. */
            if (st && node->joint_parent_index >= 0) return false;
            if (drawable == 0) {
                the_node = node;
                the_prim = prim;
            } else {
                /* Multi-primitive (a mesh split into trunk + foliage material
                 * groups — the common real-content case): allowed ONLY when every
                 * drawable primitive shares the SAME node-local transform, so ONE
                 * folded render matrix (entity_world * node_lt) places them all.
                 * The GPU-cull draw path emits one indirect draw PER primitive
                 * (each sharing the run's survivor count).  Differing transforms
                 * (assembled cars / "pack" scenes) fall to the CPU path.  (Thin
                 * overlapping foliage sees inherent compaction-reorder z-fight vs
                 * the CPU path — an order-undefined artifact, not a bug; opaque
                 * props render at parity — see box vs grass in memory.) */
                if (memcmp(&node->local_transform, &the_node->local_transform,
                           sizeof(jce_mat4)) != 0)
                    return false;
            }
            drawable++;
        }
    }
    if (drawable < 1 || !the_node || !the_prim) return false;
    if (out_node_lt) *out_node_lt = the_node->local_transform;
    return true;
}

/* Number of drawable (static / non-skinned) primitives jce_model_gpu_instanceable
 * would submit (>1 = a shared-node multi-primitive model).  0 when not
 * GPU-instanceable. */
uint32_t jce_model_gpu_drawable_count(const JceModel *model)
{
    if (!jce_model_gpu_instanceable(model, NULL)) return 0;
    uint32_t drawable = 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;
            if (!sm && !st) continue;
            drawable++;
        }
    }
    return drawable;
}

/* Index count of the `which`-th drawable primitive (0-based, in the same order
 * jce_model_gpu_drawable_count walks).  Fills that primitive's GPU indirect draw
 * args' numIndices.  Returns 0 if `which` is out of range. */
uint32_t jce_model_gpu_primitive_index_count(const JceModel *model, uint32_t which)
{
    if (!model) return 0;
    uint32_t drawable = 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;
            if (!sm && !st) continue;
            if (drawable == which) {
                if (sm) return jce_skinned_mesh_index_count(sm);
                if (st) return jce_mesh_index_count(st);
            }
            drawable++;
        }
    }
    return 0;
}

/* Static mesh of the `which`-th drawable primitive (same walk order as
 * jce_model_gpu_primitive_index_count), for CPU-side instancing paths (e.g. the
 * velocity/depth prepass render queue) that need the raw VB/IB handles.  Returns
 * NULL if `which` is out of range or that primitive isn't a static mesh. */
JceMesh *jce_model_gpu_primitive_mesh(const JceModel *model, uint32_t which)
{
    if (!model) return NULL;
    uint32_t drawable = 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;
            if (!sm && !st) continue;
            if (drawable == which) return st;   /* static-only for the RQ path */
            drawable++;
        }
    }
    return NULL;
}

/* Non-skinned SkinnedMesh of the `which`-th drawable primitive (same walk order
 * as jce_model_gpu_primitive_index_count).  This is the LOD carrier — it holds
 * the per-LOD reduced index buffers (jce_skinned_mesh_lod_ibh) that the GPU
 * LOD-in-cull path (千万 S4) binds per distance band while sharing one vertex
 * buffer.  Returns NULL when `which` is out of range or that primitive has no
 * static-LOD skinned mesh (e.g. a pure static_mesh prim). */
const JceSkinnedMesh *jce_model_gpu_primitive_skinned_mesh(const JceModel *model,
                                                           uint32_t which)
{
    if (!model) return NULL;
    uint32_t drawable = 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;
            if (!sm && !st) continue;
            if (drawable == which) return sm;   /* the LOD-carrying skinned mesh */
            drawable++;
        }
    }
    return NULL;
}

bool jce_model_any_emissive(const JceModel *model)
{
    if (!model) return false;
    for (uint32_t m = 0; m < model->num_materials; m++) {
        const JcePbrMaterial *mat = &model->materials[m];
        if (jce_texture_valid(mat->emissive_map)) return true;
        if (mat->emissive_factor[0] != 0.0f || mat->emissive_factor[1] != 0.0f ||
            mat->emissive_factor[2] != 0.0f) return true;
    }
    return false;
}

uint32_t jce_model_skinned_primitive_count(const JceModel *model)
{
    if (!model) return 0;
    uint32_t count = 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceSkinnedMesh *sm = node->primitives[p].skinned_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) count++;
        }
    }
    return count;
}

bool jce_model_skinned_primitive_buffers(const JceModel *model, uint32_t which,
                                         uint32_t *out_vbh, uint32_t *out_ibh,
                                         uint32_t *out_index_count)
{
    if (!model) return false;
    uint32_t idx = 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceSkinnedMesh *sm = node->primitives[p].skinned_mesh;
            if (!sm || !jce_skinned_mesh_is_skinned(sm)) continue;
            if (idx++ != which) continue;
            if (out_vbh)         *out_vbh         = jce_skinned_mesh_get_vbh(sm);
            if (out_ibh)         *out_ibh         = jce_skinned_mesh_get_ibh(sm);
            if (out_index_count) *out_index_count = jce_skinned_mesh_index_count(sm);
            return true;
        }
    }
    return false;
}

/* GPU-driven instanced draw of a single-primitive model: binds the primitive's
 * VB/IB + material once and submits ONE instanced draw sourcing per-instance
 * model matrices from the compute-written visible dynamic vertex buffer
 * (vs_pbr_inst reads i_data0..3).  `visible_vb` is a bgfx dynamic_vertex_buffer
 * handle index.  `count` is the upper-bound instance count (the GPU cull leaves
 * the tail zeroed → degenerate).  No-op unless jce_model_gpu_instanceable. */
void jce_model_draw_instanced_from_buffer(const JceModel *model,
                                          const JceRenderer *r, uint16_t view_id,
                                          uint16_t visible_vb,
                                          uint32_t start, uint32_t count,
                                          uint32_t which)
{
    if (!model || !r || count == 0 || visible_vb == UINT16_MAX) return;
    JceShaderHandle prog_inst = jce_renderer_get_program_pbr_inst(r);
    if (prog_inst.idx == UINT16_MAX) return;
    const bgfx_program_handle_t bgfx_prog = { (uint16_t)prog_inst.idx };
    const bgfx_dynamic_vertex_buffer_handle_t vis = { visible_vb };

    uint32_t drawable = 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;
            if (!sm && !st) continue;
            if (drawable++ != which) continue;   /* draw only the which-th */

            const JcePbrMaterial *mat = NULL;
            if (prim->material_index < model->num_materials)
                mat = &model->materials[prim->material_index];
            if (s_material_override)
                mat = s_material_override;
            JcePbrMaterial checker_mat;
            if (s_albedo_checker && mat && !jce_texture_valid(mat->albedo_map)) {
                checker_mat = *mat;
                checker_mat.normal_scale =
                    -fmaxf(fabsf(checker_mat.normal_scale), 0.0001f);
                mat = &checker_mat;
            }
            if (mat)
                jce_pbr_material_bind(mat, r, view_id);

            if (sm) {
                jce_skinned_mesh_submit(sm, r, view_id);   /* binds VB/IB/state */
            } else {
                bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(st) };
                bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(st) };
                bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
                if (ibh.idx != UINT16_MAX)
                    bgfx_set_index_buffer(ibh, 0, jce_mesh_index_count(st));
                bgfx_set_state(BGFX_STATE_DEFAULT, 0);
            }

            /* Re-bind the instance source per primitive (consumed by submit). */
            bgfx_set_instance_data_from_dynamic_vertex_buffer(vis, start, count);
            if (s_pre_submit_cb)
                s_pre_submit_cb(s_pre_submit_user, view_id);
            bgfx_submit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);
            return;   /* single drawable primitive only */
        }
    }
}

uint32_t jce_model_gpu_index_count(const JceModel *model)
{
    if (!model) return 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;
            if (sm) return jce_skinned_mesh_index_count(sm);
            if (st) return jce_mesh_index_count(st);
        }
    }
    return 0;
}

/* GPU-driven INDIRECT instanced color draw: identical primitive/material binding
 * to jce_model_draw_instanced_from_buffer, but the per-instance count + start
 * come from the GPU-written indirect args instead of CPU values.  The visible
 * buffer is bound as the instance source from slot 0 (UINT32_MAX count = whole
 * buffer); the indirect arg's startInstance selects this run's partition.  bgfx
 * IGNORES the index/vertex counts set via set_index/vertex_buffer when submitting
 * indirect — the counts come from the indirect element — so the bound counts here
 * only matter for binding the handles. */
void jce_model_draw_indirect_from_buffer(const JceModel *model,
                                         const JceRenderer *r, uint16_t view_id,
                                         uint16_t visible_vb,
                                         uint16_t indirect_buf,
                                         uint32_t indirect_el,
                                         uint32_t which)
{
    if (!model || !r || visible_vb == UINT16_MAX || indirect_buf == UINT16_MAX)
        return;
    JceShaderHandle prog_inst = jce_renderer_get_program_pbr_inst(r);
    if (prog_inst.idx == UINT16_MAX) return;
    const bgfx_program_handle_t bgfx_prog = { (uint16_t)prog_inst.idx };
    const bgfx_dynamic_vertex_buffer_handle_t vis = { visible_vb };
    const bgfx_indirect_buffer_handle_t ind = { indirect_buf };

    uint32_t drawable = 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;
            if (!sm && !st) continue;
            if (drawable++ != which) continue;   /* draw only the which-th */

            const JcePbrMaterial *mat = NULL;
            if (prim->material_index < model->num_materials)
                mat = &model->materials[prim->material_index];
            if (s_material_override)
                mat = s_material_override;
            JcePbrMaterial checker_mat;
            if (s_albedo_checker && mat && !jce_texture_valid(mat->albedo_map)) {
                checker_mat = *mat;
                checker_mat.normal_scale =
                    -fmaxf(fabsf(checker_mat.normal_scale), 0.0001f);
                mat = &checker_mat;
            }
            if (mat)
                jce_pbr_material_bind(mat, r, view_id);

            if (sm) {
                jce_skinned_mesh_submit(sm, r, view_id);   /* binds VB/IB/state */
            } else {
                bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(st) };
                bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(st) };
                bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
                if (ibh.idx != UINT16_MAX)
                    bgfx_set_index_buffer(ibh, 0, jce_mesh_index_count(st));
                bgfx_set_state(BGFX_STATE_DEFAULT, 0);
            }

            /* Bind the compacted instance stream from slot 0; the indirect arg's
             * startInstance picks this run's partition. */
            bgfx_set_instance_data_from_dynamic_vertex_buffer(vis, 0, UINT32_MAX);
            if (s_pre_submit_cb)
                s_pre_submit_cb(s_pre_submit_user, view_id);
            /* One indirect draw element (this run), starting at indirect_el. */
            bgfx_submit_indirect(view_id, bgfx_prog, ind, indirect_el, 1, 0,
                                 BGFX_DISCARD_ALL);
            return;   /* single drawable primitive only */
        }
    }
}

/* GPU-driven DEPTH-ONLY siblings of jce_model_draw_*_from_buffer (roadmap #18
 * extended to the CSM shadow cascades).  Identical primitive binding to the
 * color versions, but: depth-only state (no material bind, no Forward+ cluster
 * pre-submit), and the SHADOW instanced program (`program_idx`, from
 * jce_renderer_get_program_shadow_inst) instead of the PBR program.  The visible
 * buffer holds one mat4 per surviving instance (TEXCOORD7..4), which vs_shadow_inst
 * reads as i_data0..3.  No-op unless jce_model_gpu_instanceable(model, NULL). */
static const uint64_t JCE_SHADOW_DRAW_STATE =
    BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
    | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA;

void jce_model_draw_shadow_instanced_from_buffer(const JceModel *model,
                                                 const JceRenderer *r,
                                                 uint16_t view_id,
                                                 uint16_t program_idx,
                                                 uint16_t visible_vb,
                                                 uint32_t start, uint32_t count,
                                                 uint32_t which)
{
    if (!model || !r || count == 0
        || visible_vb == UINT16_MAX || program_idx == UINT16_MAX) return;
    const bgfx_program_handle_t bgfx_prog = { program_idx };
    const bgfx_dynamic_vertex_buffer_handle_t vis = { visible_vb };

    uint32_t drawable = 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;
            if (!st) continue;   /* GPU shadow path: static primitives only */
            if (drawable++ != which) continue;   /* cast only the which-th */

            bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(st) };
            bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(st) };
            bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
            if (ibh.idx != UINT16_MAX)
                bgfx_set_index_buffer(ibh, 0, jce_mesh_index_count(st));
            bgfx_set_state(JCE_SHADOW_DRAW_STATE, 0);
            bgfx_set_instance_data_from_dynamic_vertex_buffer(vis, start, count);
            bgfx_submit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);
            return;   /* cast the which-th primitive */
        }
    }
}

void jce_model_draw_shadow_indirect_from_buffer(const JceModel *model,
                                                const JceRenderer *r,
                                                uint16_t view_id,
                                                uint16_t program_idx,
                                                uint16_t visible_vb,
                                                uint16_t indirect_buf,
                                                uint32_t indirect_el,
                                                uint32_t which)
{
    if (!model || !r || visible_vb == UINT16_MAX
        || indirect_buf == UINT16_MAX || program_idx == UINT16_MAX) return;
    const bgfx_program_handle_t bgfx_prog = { program_idx };
    const bgfx_dynamic_vertex_buffer_handle_t vis = { visible_vb };
    const bgfx_indirect_buffer_handle_t ind = { indirect_buf };

    uint32_t drawable = 0;
    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;
            if (!st) continue;   /* GPU shadow path: static primitives only */
            if (drawable++ != which) continue;   /* cast only the which-th */

            bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(st) };
            bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(st) };
            bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
            if (ibh.idx != UINT16_MAX)
                bgfx_set_index_buffer(ibh, 0, jce_mesh_index_count(st));
            bgfx_set_state(JCE_SHADOW_DRAW_STATE, 0);
            /* Instance source from slot 0; the indirect arg's startInstance picks
             * this run's compacted partition. */
            bgfx_set_instance_data_from_dynamic_vertex_buffer(vis, 0, UINT32_MAX);
            bgfx_submit_indirect(view_id, bgfx_prog, ind, indirect_el, 1, 0,
                                 BGFX_DISCARD_ALL);
            return;   /* cast the which-th primitive */
        }
    }
}

void jce_model_draw_shadow(const JceModel *model,
                           const JceRenderer *r, uint16_t view_id,
                           const jce_mat4 *transform,
                           const jce_mat4 *joint_matrices,
                           uint32_t num_joints)
{
    if (!model || !r) return;

    /* Depth-only mirror of jce_model_draw: no materials are bound (the
       shadow programs sample nothing), and skinned primitives reuse the
       SAME world-space bone palette the color pass uploads, so the cast
       silhouette deforms in lock-step with the lit mesh. */
    const JceShaderHandle prog_skinned = jce_renderer_get_program_shadow_skinned(r);
    const JceShaderHandle prog_static  = jce_renderer_get_program_shadow(r);

    jce_mat4 identity = jce_m4_identity();
    const jce_mat4 *root = transform ? transform : &identity;

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        jce_mat4 world = jce_m4_multiply(root, &node->local_transform);

        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];

            if (prim->skinned_mesh) {
                if (jce_skinned_mesh_is_skinned(prim->skinned_mesh)) {
                    if (joint_matrices && num_joints > 0) {
                        jce_mat4 world_bones[256];
                        uint32_t nb = jce_skin_build_world_palette(
                            root, joint_matrices, num_joints, world_bones, 256);
                        jce_skinned_mesh_set_bones(world_bones, nb);
                    } else if (model->skeleton) {
                        /* No live animation: rasterize the rest/bind pose. */
                        uint32_t num_j = jce_skeleton_joint_count(model->skeleton);
                        if (num_j > 256) num_j = 256;
                        jce_mat4 bind_pose[256];
                        jce_skeleton_evaluate(model->skeleton, NULL, bind_pose, num_j);
                        uint32_t nb = jce_skin_build_world_palette(
                            root, bind_pose, num_j, bind_pose, num_j);
                        jce_skinned_mesh_set_bones(bind_pose, nb);
                    } else {
                        bgfx_set_transform(world.raw[0], 1);
                    }
                    jce_skinned_mesh_submit_shadow(prim->skinned_mesh, r,
                                                   view_id, prog_skinned);
                } else {
                    /* Static-PBR primitive (tangents, no joints): respect
                       joint-parent attachment, depth via the static program
                       (vs_shadow consumes only a_position). */
                    jce_mat4 static_world = compute_static_node_world(
                        model, node, root, &world, joint_matrices, num_joints);
                    bgfx_set_transform(static_world.raw[0], 1);
                    /* In-asset auto-LOD: cast the SAME reduced silhouette the
                     * color pass drew so shadows match the rendered LOD.  Level
                     * 0 / no LOD => identical to the base shadow submit. */
                    if (s_draw_lod_level > 0)
                        jce_skinned_mesh_submit_shadow_lod(prim->skinned_mesh, r,
                                                           view_id, prog_static,
                                                           s_draw_lod_level - 1);
                    else
                        jce_skinned_mesh_submit_shadow(prim->skinned_mesh, r,
                                                       view_id, prog_static);
                }
            } else if (prim->static_mesh) {
                jce_mat4 static_world = compute_static_node_world(
                    model, node, root, &world, joint_matrices, num_joints);
                bgfx_set_transform(static_world.raw[0], 1);
                jce_mesh_submit_shadow(prim->static_mesh, r, view_id);
            }
        }
    }
}

/* GPU-instanced depth-only mirror of jce_model_draw_shadow: rasterizes the same
 * static (non-skinned) primitives for `count` instances in one submit per
 * primitive via the shadow_inst program (vs_shadow_inst reads i_data0..3 as the
 * world matrix).  Skinned primitives are not instanceable (per-instance bone
 * palette) and are skipped here — callers must draw those per-entity.  No
 * material is bound (depth-only). */
void jce_model_draw_shadow_instanced(const JceModel *model,
                                     const JceRenderer *r, uint16_t view_id,
                                     const jce_mat4 *roots, uint32_t count)
{
    if (!model || !r || !roots || count == 0) return;

    JceShaderHandle prog_inst = jce_renderer_get_program_shadow_inst(r);
    if (count == 1 || prog_inst.idx == UINT16_MAX) {
        for (uint32_t i = 0; i < count; i++)
            jce_model_draw_shadow(model, r, view_id, &roots[i], NULL, 0);
        return;
    }
    const bgfx_program_handle_t bgfx_prog = { (uint16_t)prog_inst.idx };
    const uint16_t stride = (uint16_t)sizeof(jce_mat4);   /* 64 bytes (4 vec4) */
    /* Same depth-only state as jce_mesh_submit_shadow (front-face cull). */
    const uint64_t shadow_state = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                                | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA;

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        jce_mat4 node_lt = node->local_transform;

        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];

            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;  /* not instanceable */
            if (!sm && !st) continue;

            uint32_t start = 0;
            while (start < count) {
                uint32_t want  = count - start;
                uint32_t avail = bgfx_get_avail_instance_data_buffer(want, stride);
                uint32_t nb    = want < avail ? want : avail;
                if (nb == 0) break;   /* no instance space left this frame */

                bgfx_instance_data_buffer_t idb;
                bgfx_alloc_instance_data_buffer(&idb, nb, stride);
                uint8_t *dst = (uint8_t *)idb.data;
                for (uint32_t i = 0; i < nb; i++) {
                    jce_mat4 world = jce_m4_multiply(&roots[start + i], &node_lt);
                    jce_mat4 sw = compute_static_node_world(model, node,
                                                            &roots[start + i],
                                                            &world, NULL, 0);
                    memcpy(dst + (size_t)i * stride, sw.raw[0], sizeof(jce_mat4));
                }

                if (sm) {
                    /* In-asset auto-LOD: an instanced shadow run binds the same
                     * reduced index set as the color run (batch keyed by
                     * (model, lod) upstream) so the cast silhouette matches.
                     * submit_lod binds VB+IB and a color state; the depth-only
                     * bgfx_set_state below overrides it (same as the base path). */
                    if (s_draw_lod_level > 0)
                        jce_skinned_mesh_submit_lod(sm, r, view_id, s_draw_lod_level - 1);
                    else
                        jce_skinned_mesh_submit(sm, r, view_id);   /* binds VB/IB, no submit */
                } else {
                    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(st) };
                    bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(st) };
                    bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
                    if (ibh.idx != UINT16_MAX)
                        bgfx_set_index_buffer(ibh, 0, jce_mesh_index_count(st));
                }
                bgfx_set_state(shadow_state, 0);   /* depth-only (override color state) */
                bgfx_set_instance_data_buffer(&idb, 0, nb);
                bgfx_submit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);

                start += nb;
            }
        }
    }
}

/* Depth-only sibling of jce_model_draw_bindpose_instanced: casts `count`
 * NON-animating (bind-pose) skinned instances of a model into a shadow view in
 * one instanced submit per skinned primitive, via the instanced SHADOW program
 * (vs_shadow_inst reads i_data0..3 as the per-instance world matrix) with
 * depth-only state and no material bind.  A bind-pose skinned mesh has its
 * vertices in model space (its bind-pose skin matrix is identity), so the
 * per-instance world matrix places the cast silhouette exactly where the color
 * bind-pose pass draws it — this collapses the per-char skinned shadow submits
 * that dominate a big crowd's cascade gather (the #1 sh_gather cost) into one
 * instanced depth draw per (model, primitive, cascade).  The complement of
 * jce_model_draw_shadow_instanced, which casts the STATIC primitives and skips
 * the skinned ones; here we cast the skinned primitives a bind-pose crowd is
 * made of.  Skinned primitives are the only ones drawn (static primitives of a
 * mixed model are handled by the static instanced shadow path). */
void jce_model_draw_bindpose_shadow_instanced(const JceModel *model,
                                              const JceRenderer *r,
                                              uint16_t view_id,
                                              const jce_mat4 *worlds,
                                              uint32_t count)
{
    if (!model || !r || !worlds || count == 0) return;
    JceShaderHandle prog_inst = jce_renderer_get_program_shadow_inst(r);
    if (prog_inst.idx == UINT16_MAX) return;
    const bgfx_program_handle_t prog = { (uint16_t)prog_inst.idx };
    const uint16_t stride = (uint16_t)sizeof(jce_mat4);   /* 64 B: world mat4 (i_data0..3) */

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            if (!sm || !jce_skinned_mesh_is_skinned(sm)) continue;  /* skinned only */

            uint32_t start = 0;
            while (start < count) {
                uint32_t want  = count - start;
                uint32_t avail = bgfx_get_avail_instance_data_buffer(want, stride);
                uint32_t nb    = want < avail ? want : avail;
                if (nb == 0) break;

                bgfx_instance_data_buffer_t idb;
                bgfx_alloc_instance_data_buffer(&idb, nb, stride);
                uint8_t *dst = (uint8_t *)idb.data;
                for (uint32_t i = 0; i < nb; i++)
                    memcpy(dst + (size_t)i * stride, worlds[start + i].raw[0], sizeof(jce_mat4));

                jce_skinned_mesh_bind_shadow(sm);   /* binds VB/tri-IB + depth state (no wireframe) */
                bgfx_set_instance_data_buffer(&idb, 0, nb);
                bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);

                start += nb;
            }
        }
    }
}

/* ANIMATED sibling of jce_model_draw_bindpose_shadow_instanced: casts `count`
 * uniquely-POSED skinned instances into a shadow view in one instanced submit
 * per skinned primitive.  Per-instance stream mirrors the color crowd draw:
 * world matrix (i_data0..3) + bone-palette base (i_data4.x) into the shared
 * per-frame bone texture (sampler stage 4 — free in the depth pass; kept equal
 * to the color program's stage so ONE uniform/binding convention serves both).
 * prog must be vs_shadow_skinned_inst + fs_shadow. */
void jce_model_draw_crowd_shadow_instanced(const JceModel *model,
                                           const JceRenderer *r,
                                           uint16_t view_id, uint16_t prog_idx,
                                           const jce_mat4 *worlds,
                                           const uint32_t *bases, uint32_t count,
                                           uint16_t s_bones_idx,
                                           uint16_t bone_tex_idx,
                                           uint16_t u_params_idx,
                                           float tex_w, float tex_h)
{
    if (!model || !r || !worlds || !bases || count == 0 || prog_idx == UINT16_MAX) return;

    const bgfx_program_handle_t prog     = { prog_idx };
    const bgfx_uniform_handle_t s_bones  = { s_bones_idx };
    const bgfx_texture_handle_t bone_tex = { bone_tex_idx };
    const bgfx_uniform_handle_t u_params = { u_params_idx };
    /* 80 B/instance: world mat4 (i_data0..3) + palette base (i_data4.x). */
    const uint16_t stride = (uint16_t)(sizeof(jce_mat4) + sizeof(jce_vec4));
    float params[4] = { tex_w, tex_h, 0.0f, 0.0f };

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            if (!sm || !jce_skinned_mesh_is_skinned(sm)) continue;  /* skinned only */

            uint32_t start = 0;
            while (start < count) {
                uint32_t want  = count - start;
                uint32_t avail = bgfx_get_avail_instance_data_buffer(want, stride);
                uint32_t nb    = want < avail ? want : avail;
                if (nb == 0) break;

                bgfx_instance_data_buffer_t idb;
                bgfx_alloc_instance_data_buffer(&idb, nb, stride);
                uint8_t *dst = (uint8_t *)idb.data;
                for (uint32_t i = 0; i < nb; i++) {
                    uint8_t *slot = dst + (size_t)i * stride;
                    memcpy(slot, worlds[start + i].raw[0], sizeof(jce_mat4)); /* i_data0..3 */
                    float base4[4] = { (float)bases[start + i], 0.0f, 0.0f, 0.0f };
                    memcpy(slot + sizeof(jce_mat4), base4, sizeof(base4));     /* i_data4 */
                }

                jce_skinned_mesh_bind_shadow(sm);   /* binds VB/tri-IB + depth state */
                bgfx_set_texture(4, s_bones, bone_tex, UINT32_MAX);
                bgfx_set_uniform(u_params, params, 1);
                bgfx_set_instance_data_buffer(&idb, 0, nb);
                bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);

                start += nb;
            }
        }
    }
}

/* VELOCITY sibling of the animated crowd draws: writes per-limb motion vectors
 * + world normals for `count` uniquely-posed skinned instances in one instanced
 * MRT submit per skinned primitive.  Dual bone textures: s_bones (stage 4, this
 * frame) + s_prevBones (stage 6, last frame at the SAME bases).  prog must be
 * vs_gbuffer_vel_skinned_inst + fs_gbuffer_vel; ctx supplies the un-jittered
 * cur/prev view*proj (set per submit — bgfx clears uniform state per draw). */
void jce_model_draw_crowd_velocity_instanced(const JceModel *model,
                                             const JceRenderer *r,
                                             uint16_t view_id, uint16_t prog_idx,
                                             const JceModelVelocityCtx *ctx,
                                             const jce_mat4 *worlds,
                                             const uint32_t *bases, uint32_t count,
                                             uint16_t s_bones_idx,
                                             uint16_t bone_tex_idx,
                                             uint16_t s_prev_bones_idx,
                                             uint16_t bone_prev_tex_idx,
                                             uint16_t u_params_idx,
                                             float tex_w, float tex_h,
                                             uint16_t u_gbuffer_mat_idx,
                                             float roughness)
{
    if (!model || !r || !ctx || !worlds || !bases || count == 0 ||
        prog_idx == UINT16_MAX) return;

    const bgfx_program_handle_t prog       = { prog_idx };
    const bgfx_uniform_handle_t s_bones    = { s_bones_idx };
    const bgfx_texture_handle_t bone_tex   = { bone_tex_idx };
    const bgfx_uniform_handle_t s_prev     = { s_prev_bones_idx };
    const bgfx_texture_handle_t prev_tex   = { bone_prev_tex_idx };
    const bgfx_uniform_handle_t u_params   = { u_params_idx };
    const bgfx_uniform_handle_t u_cur_vp   = { ctx->u_cur_vp_idx };
    const bgfx_uniform_handle_t u_prev_vp  = { ctx->u_prev_vp_idx };
    const bgfx_uniform_handle_t u_gmat     = { u_gbuffer_mat_idx };
    const uint16_t stride = (uint16_t)(sizeof(jce_mat4) + sizeof(jce_vec4));
    float params[4] = { tex_w, tex_h, 0.0f, 0.0f };
    float gmat[4]   = { roughness, 0.0f, 0.0f, 0.0f };

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            if (!sm || !jce_skinned_mesh_is_skinned(sm)) continue;  /* skinned only */

            uint32_t start = 0;
            while (start < count) {
                uint32_t want  = count - start;
                uint32_t avail = bgfx_get_avail_instance_data_buffer(want, stride);
                uint32_t nb    = want < avail ? want : avail;
                if (nb == 0) break;

                bgfx_instance_data_buffer_t idb;
                bgfx_alloc_instance_data_buffer(&idb, nb, stride);
                uint8_t *dst = (uint8_t *)idb.data;
                for (uint32_t i = 0; i < nb; i++) {
                    uint8_t *slot = dst + (size_t)i * stride;
                    memcpy(slot, worlds[start + i].raw[0], sizeof(jce_mat4)); /* i_data0..3 */
                    float base4[4] = { (float)bases[start + i], 0.0f, 0.0f, 0.0f };
                    memcpy(slot + sizeof(jce_mat4), base4, sizeof(base4));     /* i_data4 */
                }

                jce_skinned_mesh_submit(sm, r, view_id);   /* binds VB/IB/state */
                bgfx_set_texture(4, s_bones, bone_tex, UINT32_MAX);
                bgfx_set_texture(6, s_prev,  prev_tex, UINT32_MAX);
                bgfx_set_uniform(u_params,  params, 1);
                bgfx_set_uniform(u_cur_vp,  ctx->cur_view_proj.raw[0],  1);
                bgfx_set_uniform(u_prev_vp, ctx->prev_view_proj.raw[0], 1);
                bgfx_set_uniform(u_gmat,    gmat, 1);
                bgfx_set_instance_data_buffer(&idb, 0, nb);
                bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);

                start += nb;
            }
        }
    }
}

/* ================================================================== */
/* TAA per-object / per-bone motion-vector draw                        */
/* ================================================================== */
/*
 * Mirror of jce_model_draw_shadow's node/skin walk that, instead of depth-only,
 * writes per-object (and per-bone for skinned) screen motion into the velocity
 * G-buffer attachment.  For each primitive it uploads BOTH the current world
 * bone palette (u_model[] via set_bones / set_transform — exactly as the color
 * pass) and the PREVIOUS world palette (u_prevBones[] / u_prevModel uniform),
 * plus the cur/prev un-jittered view*proj.  The velocity shaders project the
 * vertex through both and emit (curNDC-prevNDC)*0.5+0.5 (matching
 * fs_motion_vec.sc).  prev_* NULL => reuse current => that source contributes
 * no motion (camera motion still shows via prev_view_proj).
 */
void jce_model_draw_velocity(const JceModel *model,
                             const JceRenderer *r, uint16_t view_id,
                             const JceModelVelocityCtx *ctx,
                             const jce_mat4 *transform,
                             const jce_mat4 *joint_matrices,
                             uint32_t num_joints,
                             const jce_mat4 *prev_transform,
                             const jce_mat4 *prev_joint_matrices,
                             uint32_t num_prev_joints)
{
    if (!model || !r || !ctx) return;
    if (ctx->static_program.idx == UINT16_MAX) return;

    jce_mat4 identity = jce_m4_identity();
    const jce_mat4 *root      = transform ? transform : &identity;
    const jce_mat4 *prev_root = prev_transform ? prev_transform : root;

    /* Frame-constant uniforms: cur/prev un-jittered view*proj. */
    bgfx_uniform_handle_t u_curvp  = { ctx->u_cur_vp_idx };
    bgfx_uniform_handle_t u_prevvp = { ctx->u_prev_vp_idx };
    bgfx_uniform_handle_t u_prevmodel = { ctx->u_prev_model_idx };
    bgfx_uniform_handle_t u_prevbones = { ctx->u_prev_bones_idx };
    if (u_curvp.idx  != UINT16_MAX) bgfx_set_uniform(u_curvp,  ctx->cur_view_proj.raw[0],  1);
    if (u_prevvp.idx != UINT16_MAX) bgfx_set_uniform(u_prevvp, ctx->prev_view_proj.raw[0], 1);

    const bool have_skinned = (ctx->skinned_program.idx != UINT16_MAX);

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        jce_mat4 world      = jce_m4_multiply(root,      &node->local_transform);
        jce_mat4 prev_world = jce_m4_multiply(prev_root, &node->local_transform);

        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];

            if (prim->skinned_mesh) {
                if (jce_skinned_mesh_is_skinned(prim->skinned_mesh) && have_skinned) {
                    /* Build the CURRENT world palette into u_model[]. */
                    if (joint_matrices && num_joints > 0) {
                        jce_mat4 world_bones[256];
                        uint32_t nb = jce_skin_build_world_palette(
                            root, joint_matrices, num_joints, world_bones, 256);
                        jce_skinned_mesh_set_bones(world_bones, nb);
                    } else if (model->skeleton) {
                        uint32_t num_j = jce_skeleton_joint_count(model->skeleton);
                        if (num_j > 256) num_j = 256;
                        jce_mat4 bind_pose[256];
                        jce_skeleton_evaluate(model->skeleton, NULL, bind_pose, num_j);
                        uint32_t nb = jce_skin_build_world_palette(
                            root, bind_pose, num_j, bind_pose, num_j);
                        jce_skinned_mesh_set_bones(bind_pose, nb);
                    } else {
                        bgfx_set_transform(world.raw[0], 1);
                    }
                    /* Build the PREVIOUS world palette into u_prevBones[].  When
                       no prev palette is supplied, reuse the current sources so
                       the bones contribute no motion (camera-only). */
                    if (u_prevbones.idx != UINT16_MAX) {
                        jce_mat4 prev_bones[256];
                        uint32_t pnb = 0;
                        if (prev_joint_matrices && num_prev_joints > 0)
                            pnb = jce_skin_build_world_palette(
                                prev_root, prev_joint_matrices, num_prev_joints,
                                prev_bones, 256);
                        else if (joint_matrices && num_joints > 0)
                            pnb = jce_skin_build_world_palette(
                                prev_root, joint_matrices, num_joints,
                                prev_bones, 256);
                        if (pnb > 0)
                            bgfx_set_uniform(u_prevbones, prev_bones->raw[0], (uint16_t)pnb);
                    }
                    jce_skinned_mesh_submit(prim->skinned_mesh, r, view_id);
                    bgfx_program_handle_t prog = { ctx->skinned_program.idx };
                    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
                } else {
                    /* Static-PBR primitive (no joints): static velocity program. */
                    jce_mat4 static_world = compute_static_node_world(
                        model, node, root, &world, joint_matrices, num_joints);
                    jce_mat4 static_prev = compute_static_node_world(
                        model, node, prev_root, &prev_world,
                        prev_joint_matrices ? prev_joint_matrices : joint_matrices,
                        prev_joint_matrices ? num_prev_joints : num_joints);
                    bgfx_set_transform(static_world.raw[0], 1);
                    if (u_prevmodel.idx != UINT16_MAX)
                        bgfx_set_uniform(u_prevmodel, static_prev.raw[0], 1);
                    jce_skinned_mesh_submit(prim->skinned_mesh, r, view_id);
                    bgfx_program_handle_t prog = { ctx->static_program.idx };
                    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
                }
            } else if (prim->static_mesh) {
                jce_mat4 static_world = compute_static_node_world(
                    model, node, root, &world, joint_matrices, num_joints);
                jce_mat4 static_prev = compute_static_node_world(
                    model, node, prev_root, &prev_world,
                    prev_joint_matrices ? prev_joint_matrices : joint_matrices,
                    prev_joint_matrices ? num_prev_joints : num_joints);
                bgfx_set_transform(static_world.raw[0], 1);
                if (u_prevmodel.idx != UINT16_MAX)
                    bgfx_set_uniform(u_prevmodel, static_prev.raw[0], 1);
                jce_mesh_submit_pbr_with_program(prim->static_mesh, r, view_id,
                                                 ctx->static_program);
            }
        }
    }
}

/* ================================================================== */
/* Morph-aware draw (FEATURE 3.1 GPU vertex-deform)                    */
/* ================================================================== */
/*
 * jce_model_draw_morphed / _shadow are byte-for-byte mirrors of the base
 * draw / draw_shadow EXCEPT that, for each skinned primitive, they ask the
 * per-instance callback for an alternate dynamic vertex buffer holding that
 * instance's CPU-morphed vertices.  When the callback returns a valid handle
 * the submit binds the dynamic VB (via jce_skinned_mesh_submit*_morphed);
 * otherwise the helpers fall through to the unchanged static submit (UINT16_MAX
 * sentinel), so vb_cb == NULL is identical to the base entrypoints.  The bone
 * palette / u_model[] upload and the chosen program are UNCHANGED — morph is a
 * pure pre-skin vertex rewrite read by the same skinned shader. */

static uint16_t morph_vb_for(JceModelMorphVbCb cb, void *user,
                             uint32_t node, uint32_t prim)
{
    return cb ? cb(user, node, prim) : (uint16_t)UINT16_MAX;
}

void jce_model_draw_morphed(const JceModel *model,
                            const JceRenderer *r, uint16_t view_id,
                            const jce_mat4 *transform,
                            const jce_mat4 *joint_matrices,
                            uint32_t num_joints,
                            JceModelMorphVbCb vb_cb, void *vb_user)
{
    if (!model || !r) return;

    jce_mat4 identity = jce_m4_identity();
    const jce_mat4 *root = transform ? transform : &identity;

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        jce_mat4 world = jce_m4_multiply(root, &node->local_transform);

        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];

            const JcePbrMaterial *mat = NULL;
            if (prim->material_index < model->num_materials)
                mat = &model->materials[prim->material_index];
            if (mat)
                jce_pbr_material_bind(mat, r, view_id);
            jce_skinned_mesh_set_submit_double_sided(mat && mat->double_sided);

            if (prim->skinned_mesh) {
                JceShaderHandle prog_handle;

                if (jce_skinned_mesh_is_skinned(prim->skinned_mesh)) {
                    if (joint_matrices && num_joints > 0) {
                        jce_mat4 world_bones[256];
                        uint32_t nb = jce_skin_build_world_palette(
                            root, joint_matrices, num_joints, world_bones, 256);
                        jce_skinned_mesh_set_bones(world_bones, nb);
                    } else if (model->skeleton) {
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
                    jce_mat4 static_world = compute_static_node_world(
                        model, node, root, &world, joint_matrices, num_joints);
                    bgfx_set_transform(static_world.raw[0], 1);
                    prog_handle = jce_renderer_get_program_pbr(r);
                }

                /* Per-instance morphed VB override (UINT16_MAX => static). */
                uint16_t dvb = morph_vb_for(vb_cb, vb_user, n, p);
                jce_skinned_mesh_submit_morphed(prim->skinned_mesh, r, view_id, dvb);

                if (s_pre_submit_cb)
                    s_pre_submit_cb(s_pre_submit_user, view_id);

                bgfx_program_handle_t bgfx_prog = { prog_handle.idx };
                bgfx_submit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);

            } else if (prim->static_mesh) {
                /* Static (non-skinned, no morph) — identical to jce_model_draw. */
                jce_mat4 static_world = compute_static_node_world(
                    model, node, root, &world, joint_matrices, num_joints);
                bgfx_set_transform(static_world.raw[0], 1);
                if (s_pre_submit_cb)
                    s_pre_submit_cb(s_pre_submit_user, view_id);
                jce_mesh_submit(prim->static_mesh, r, view_id);
            }
        }
    }
    jce_skinned_mesh_set_submit_double_sided(false);
}

void jce_model_draw_morphed_shadow(const JceModel *model,
                                   const JceRenderer *r, uint16_t view_id,
                                   const jce_mat4 *transform,
                                   const jce_mat4 *joint_matrices,
                                   uint32_t num_joints,
                                   JceModelMorphVbCb vb_cb, void *vb_user)
{
    if (!model || !r) return;

    const JceShaderHandle prog_skinned = jce_renderer_get_program_shadow_skinned(r);
    const JceShaderHandle prog_static  = jce_renderer_get_program_shadow(r);

    jce_mat4 identity = jce_m4_identity();
    const jce_mat4 *root = transform ? transform : &identity;

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        jce_mat4 world = jce_m4_multiply(root, &node->local_transform);

        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];

            if (prim->skinned_mesh) {
                uint16_t dvb = morph_vb_for(vb_cb, vb_user, n, p);
                if (jce_skinned_mesh_is_skinned(prim->skinned_mesh)) {
                    if (joint_matrices && num_joints > 0) {
                        jce_mat4 world_bones[256];
                        uint32_t nb = jce_skin_build_world_palette(
                            root, joint_matrices, num_joints, world_bones, 256);
                        jce_skinned_mesh_set_bones(world_bones, nb);
                    } else if (model->skeleton) {
                        uint32_t num_j = jce_skeleton_joint_count(model->skeleton);
                        if (num_j > 256) num_j = 256;
                        jce_mat4 bind_pose[256];
                        jce_skeleton_evaluate(model->skeleton, NULL, bind_pose, num_j);
                        uint32_t nb = jce_skin_build_world_palette(
                            root, bind_pose, num_j, bind_pose, num_j);
                        jce_skinned_mesh_set_bones(bind_pose, nb);
                    } else {
                        bgfx_set_transform(world.raw[0], 1);
                    }
                    jce_skinned_mesh_submit_shadow_morphed(prim->skinned_mesh, r,
                                                           view_id, prog_skinned, dvb);
                } else {
                    jce_mat4 static_world = compute_static_node_world(
                        model, node, root, &world, joint_matrices, num_joints);
                    bgfx_set_transform(static_world.raw[0], 1);
                    jce_skinned_mesh_submit_shadow_morphed(prim->skinned_mesh, r,
                                                           view_id, prog_static, dvb);
                }
            } else if (prim->static_mesh) {
                jce_mat4 static_world = compute_static_node_world(
                    model, node, root, &world, joint_matrices, num_joints);
                bgfx_set_transform(static_world.raw[0], 1);
                jce_mesh_submit_shadow(prim->static_mesh, r, view_id);
            }
        }
    }
}

/* ================================================================== */
/* Wireframe overlay (selection / editor tooling)                      */
/* ================================================================== */

void jce_model_submit_wireframe_overlay(const JceModel *model,
                                         const JceRenderer *r,
                                         uint16_t view_id,
                                         const jce_mat4 *transform,
                                         const jce_mat4 *joint_matrices,
                                         uint32_t num_joints)
{
    if (!model || !r) return;

    jce_mat4 identity = jce_m4_identity();
    const jce_mat4 *root = transform ? transform : &identity;

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        jce_mat4 world = jce_m4_multiply(root, &node->local_transform);

        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];

            if (prim->skinned_mesh) {
                if (jce_skinned_mesh_is_skinned(prim->skinned_mesh)) {
                    if (joint_matrices && num_joints > 0) {
                        jce_mat4 world_bones[256];
                        uint32_t nb = jce_skin_build_world_palette(
                            root, joint_matrices, num_joints, world_bones, 256);
                        jce_skinned_mesh_set_bones(world_bones, nb);
                    } else if (model->skeleton) {
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
                } else {
                    jce_mat4 static_world = compute_static_node_world(
                        model, node, root, &world, joint_matrices, num_joints);
                    bgfx_set_transform(static_world.raw[0], 1);
                }
                jce_skinned_mesh_submit_wireframe_overlay(prim->skinned_mesh, r, view_id);
            } else if (prim->static_mesh) {
                jce_mat4 static_world = compute_static_node_world(
                    model, node, root, &world, joint_matrices, num_joints);
                bgfx_set_transform(static_world.raw[0], 1);
                jce_mesh_submit_wireframe_overlay(prim->static_mesh, r, view_id);
            }
        }
    }
}

void jce_model_submit_pick_id(const JceModel *model,
                              const JceRenderer *r,
                              uint16_t view_id,
                              const jce_mat4 *transform,
                              const jce_mat4 *joint_matrices,
                              uint32_t num_joints,
                              JceShaderHandle static_program,
                              JceShaderHandle skinned_program)
{
    if (!model || !r)
        return;

    jce_mat4 identity = jce_m4_identity();
    const jce_mat4 *root = transform ? transform : &identity;

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        jce_mat4 world = jce_m4_multiply(root, &node->local_transform);

        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];

            bool double_sided = false;
            if (prim->material_index < model->num_materials)
                double_sided = model->materials[prim->material_index].double_sided;

            if (prim->skinned_mesh) {
                if (jce_skinned_mesh_is_skinned(prim->skinned_mesh)) {
                    if (joint_matrices && num_joints > 0) {
                        jce_mat4 world_bones[256];
                        uint32_t nb = jce_skin_build_world_palette(
                            root, joint_matrices, num_joints, world_bones, 256);
                        jce_skinned_mesh_set_bones(world_bones, nb);
                    } else if (model->skeleton) {
                        uint32_t num_j = jce_skeleton_joint_count(model->skeleton);
                        if (num_j > 256)
                            num_j = 256;
                        jce_mat4 bind_pose[256];
                        jce_skeleton_evaluate(model->skeleton, NULL, bind_pose, num_j);
                        for (uint32_t bi = 0; bi < num_j; bi++)
                            bind_pose[bi] = jce_m4_multiply(root, &bind_pose[bi]);
                        jce_skinned_mesh_set_bones(bind_pose, num_j);
                    } else {
                        bgfx_set_transform(world.raw[0], 1);
                    }
                    jce_skinned_mesh_submit_pick_id(prim->skinned_mesh, r, view_id,
                                                    skinned_program, double_sided);
                } else {
                    jce_mat4 static_world = compute_static_node_world(
                        model, node, root, &world, joint_matrices, num_joints);
                    bgfx_set_transform(static_world.raw[0], 1);
                    jce_skinned_mesh_submit_pick_id(prim->skinned_mesh, r, view_id,
                                                    static_program, double_sided);
                }
            } else if (prim->static_mesh) {
                jce_mat4 static_world = compute_static_node_world(
                    model, node, root, &world, joint_matrices, num_joints);
                bgfx_set_transform(static_world.raw[0], 1);
                jce_mesh_submit_pick_id(prim->static_mesh, r, view_id,
                                        static_program, double_sided);
            }
        }
    }
}
