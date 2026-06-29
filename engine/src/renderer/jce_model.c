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
            drawable++;
            if (drawable > 1) return false;   /* multi-primitive → CPU path */
            the_node = node;
            the_prim = prim;
        }
    }
    if (drawable != 1 || !the_node || !the_prim) return false;
    /* A joint-parented static prop needs compute_static_node_world per-instance
     * (joint attachment), which the shared instance buffer cannot carry — keep
     * those on the CPU path. */
    if (the_prim->static_mesh && the_node->joint_parent_index >= 0) return false;
    if (out_node_lt) *out_node_lt = the_node->local_transform;
    return true;
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
                                          uint32_t start, uint32_t count)
{
    if (!model || !r || count == 0 || visible_vb == UINT16_MAX) return;
    JceShaderHandle prog_inst = jce_renderer_get_program_pbr_inst(r);
    if (prog_inst.idx == UINT16_MAX) return;
    const bgfx_program_handle_t bgfx_prog = { (uint16_t)prog_inst.idx };
    const bgfx_dynamic_vertex_buffer_handle_t vis = { visible_vb };

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;
            if (!sm && !st) continue;

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
                                         uint32_t indirect_el)
{
    if (!model || !r || visible_vb == UINT16_MAX || indirect_buf == UINT16_MAX)
        return;
    JceShaderHandle prog_inst = jce_renderer_get_program_pbr_inst(r);
    if (prog_inst.idx == UINT16_MAX) return;
    const bgfx_program_handle_t bgfx_prog = { (uint16_t)prog_inst.idx };
    const bgfx_dynamic_vertex_buffer_handle_t vis = { visible_vb };
    const bgfx_indirect_buffer_handle_t ind = { indirect_buf };

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;
            if (!sm && !st) continue;

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
                                                 uint32_t start, uint32_t count)
{
    if (!model || !r || count == 0
        || visible_vb == UINT16_MAX || program_idx == UINT16_MAX) return;
    const bgfx_program_handle_t bgfx_prog = { program_idx };
    const bgfx_dynamic_vertex_buffer_handle_t vis = { visible_vb };

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;
            if (!st) continue;   /* GPU shadow path: static primitives only */

            bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(st) };
            bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(st) };
            bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
            if (ibh.idx != UINT16_MAX)
                bgfx_set_index_buffer(ibh, 0, jce_mesh_index_count(st));
            bgfx_set_state(JCE_SHADOW_DRAW_STATE, 0);
            bgfx_set_instance_data_from_dynamic_vertex_buffer(vis, start, count);
            bgfx_submit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);
            return;   /* single drawable primitive only */
        }
    }
}

void jce_model_draw_shadow_indirect_from_buffer(const JceModel *model,
                                                const JceRenderer *r,
                                                uint16_t view_id,
                                                uint16_t program_idx,
                                                uint16_t visible_vb,
                                                uint16_t indirect_buf,
                                                uint32_t indirect_el)
{
    if (!model || !r || visible_vb == UINT16_MAX
        || indirect_buf == UINT16_MAX || program_idx == UINT16_MAX) return;
    const bgfx_program_handle_t bgfx_prog = { program_idx };
    const bgfx_dynamic_vertex_buffer_handle_t vis = { visible_vb };
    const bgfx_indirect_buffer_handle_t ind = { indirect_buf };

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const JceSkinnedMesh *sm = prim->skinned_mesh;
            JceMesh              *st = prim->static_mesh;
            if (sm && jce_skinned_mesh_is_skinned(sm)) continue;
            if (!st) continue;   /* GPU shadow path: static primitives only */

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
            return;   /* single drawable primitive only */
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
