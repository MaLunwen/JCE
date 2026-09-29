/*
 * jce_sr_morph.c  The scene renderer's blendshape (morph target) pass.
 *
 * Split out of jce_sr_anim.c, unchanged, when that file crossed AGENTS.md
 * §11's 3000-line cap.  The seam is real and not arbitrary: everything here
 * answers one question -- what shape is this instance's mesh THIS frame --
 * while the file it left is about skeletons, IK, retargeting and clips.
 *
 * Three steps, in this order:
 *
 *   1. RESOLVE.  Combine the clip-driven morph-weight track with the entity's
 *      authored JceMorphWeights component (jce_morph_resolve_weights) into the
 *      final per-target weight vector, cached on the instance.
 *   2. DEFORM.  For each morph-bearing primitive, write the deformed vertices
 *      into a per-instance dynamic vertex buffer -- with a compute dispatch
 *      where the backend has compute, and with jce_morph_apply on this thread
 *      where it does not.
 *   3. DRAW.  sr_morph_vb_cb hands the draw path the deformed buffer for a
 *      (node, prim), or UINT16_MAX to fall through to the static one.
 *
 * Gating keeps legacy content byte-identical: no morph data, no component and
 * no track means morph_count stays 0, no deform runs, no dynamic VB is
 * created, and the draw path takes the static buffer it always took.
 */

#include "jce_sr_internal.h"
#include <jce/renderer/jce_model.h>   /* morph-weight anim tracks */
#include "middleware/animation/jce_morph_gpu.h"   /* GPU blendshape deform */

#include <jce/resource/jce_pak_loader.h>

/* FEATURE 3.1 last-mile — per-instance morph (blendshape) weight resolution.
 *
 * For each skinned entity whose model carries morph-target data, compute the
 * FINAL per-target weights = the clip-driven morph-weight track (sampled at the
 * active clip's current time) OVERRIDDEN per-target by the entity's authored
 * static JceMorphWeights component, via jce_morph_resolve_weights.  The result
 * is cached on the instance (ai->morph_weights / morph_count).
 *
 * Gating keeps legacy content byte-identical: morph_count stays 0 (and the
 * stored weights untouched) unless the model actually has morph targets AND
 * either a JceMorphWeights component or an imported weight track is present.
 *
 * Once weights are resolved, sr_deform_morph_prims runs the CPU pre-skin deform
 * (jce_morph_apply) into per-instance dynamic vertex buffers that the UNCHANGED
 * skinned program reads; the bone palette / shaders are untouched.  No morph
 * component => morph_count 0 => no deform => no dynamic VB => byte-identical. */

/* Per-instance morph-VB override callback handed to jce_model_draw_morphed /
 * _shadow.  Given (node, prim), returns the live dynamic-VB handle idx for that
 * primitive, or UINT16_MAX to fall through to the static skinned VB.  The same
 * callback (and the same ai) MUST drive both color and shadow so the cast
 * silhouette matches the morphed, lit mesh. */
uint16_t sr_morph_vb_cb(void *user, uint32_t node, uint32_t prim)
{
    const SrAnimInstance *ai = (const SrAnimInstance *)user;
    if (!ai) return (uint16_t)UINT16_MAX;
    for (uint16_t s = 0; s < ai->morph_vb_count; s++) {
        if (ai->morph_vb_node[s] == node && ai->morph_vb_prim[s] == prim)
            return ai->morph_vb[s].idx;   /* UINT16_MAX if not yet created */
    }
    return (uint16_t)UINT16_MAX;
}

/* Lazy-find (or allocate) the per-instance morph-VB slot for a (node, prim).
 * Returns the slot index, or -1 if the bounded array is full. */
static int sr_morph_vb_slot(SrAnimInstance *ai, uint32_t node, uint32_t prim)
{
    for (uint16_t s = 0; s < ai->morph_vb_count; s++) {
        if (ai->morph_vb_node[s] == node && ai->morph_vb_prim[s] == prim)
            return (int)s;
    }
    if (ai->morph_vb_count >= SR_MORPH_PRIM_MAX) return -1;
    int s = (int)ai->morph_vb_count++;
    ai->morph_vb_node[s] = node;
    ai->morph_vb_prim[s] = prim;
    /* handle stays BGFX_INVALID_HANDLE until the first upload creates it */
    return s;
}

/* Pre-skin deform: for each morph-bearing primitive, morph the retained base
 * verts by ai->morph_weights into the instance's dynamic VB.  Dirty-gated
 * against ai->morph_last_weights so static weights cost nothing after the
 * first upload.  LOD-guarded: only deform when the bound mesh's vertex count
 * equals the morph delta vertex count.
 *
 * TWO EVALUATORS, ONE FORMULA.  Where the backend has compute the deform is a
 * dispatch (jce_morph_gpu_deform) reading device-resident base vertices and
 * deltas that every instance of the model SHARES; where it does not -- the
 * desktop GL 120 profile, by design -- it is jce_morph_apply on this thread,
 * over a full per-instance copy of the vertex array.  cs_morph_deform.sc is
 * written against jce_morph_apply line for line so the two agree.
 *
 * The choice is made PER PRIMITIVE and BEFORE the output buffer is created,
 * because the two paths need different buffer flags and a COMPUTE_WRITE
 * buffer cannot be written from the CPU.  See morph_vb_gpu_mask. */
void sr_deform_morph_prims(JceSceneRenderer *sr, SrAnimInstance *ai)
{
    if (!ai || !ai->model || ai->morph_count == 0) return;

    /* Dirty token: skip the whole deform when the resolved weights are
     * unchanged from the last upload (count + every value). */
    bool dirty = (ai->morph_last_count != (int)ai->morph_count);
    if (!dirty) {
        for (uint32_t t = 0; t < ai->morph_count; t++) {
            if (ai->morph_last_weights[t] != ai->morph_weights[t]) { dirty = true; break; }
        }
    }
    if (!dirty) return;

    uint32_t nnodes = jce_model_node_count(ai->model);
    for (uint32_t n = 0; n < nnodes; n++) {
        uint32_t nprims = jce_model_node_prim_count(ai->model, n);
        for (uint32_t p = 0; p < nprims; p++) {
            const JceMorphData *md = jce_model_prim_morph(ai->model, n, p);
            if (!md) continue;   /* not a morph-bearing prim */

            uint32_t delta_verts = jce_morph_vertex_count(md);
            uint32_t mesh_verts  = jce_model_prim_vertex_count(ai->model, n, p);
            /* LOD/topology guard: deform only when the bound mesh vertex count
             * matches the morph delta vertex count.  A mismatch (e.g. a future
             * LOD-substituted mesh) disables morph for that prim rather than
             * indexing past the deltas. */
            if (delta_verts == 0 || delta_verts != mesh_verts) continue;

            const JceSkinnedMesh *sm = jce_model_prim_skinned_mesh(ai->model, n, p);
            if (!sm) continue;
            const void *base = jce_skinned_mesh_base_verts(sm);
            uint32_t stride  = jce_skinned_mesh_stride(sm);
            const void *layout = jce_skinned_mesh_layout(sm);
            if (!base || stride == 0 || !layout) continue;  /* not retained */

            int slot = sr_morph_vb_slot(ai, n, p);
            if (slot < 0) continue;   /* per-instance VB array full */

            /* Lazily create the dynamic VB with the SAME layout as the static
             * VB -- and with the flags the path that will WRITE it needs.
             * jce_morph_gpu_prepare answers that question first and uploads
             * the shared device buffers while it is at it. */
            if (ai->morph_vb[slot].idx == UINT16_MAX) {
                const bool gpu = jce_morph_gpu_prepare(sr ? sr->pak : NULL,
                                                       md, sm, mesh_verts);
                ai->morph_vb[slot] = bgfx_create_dynamic_vertex_buffer(
                    mesh_verts, (const bgfx_vertex_layout_t *)layout,
                    gpu ? JCE_MORPH_GPU_OUT_FLAGS : BGFX_BUFFER_NONE);
                if (ai->morph_vb[slot].idx == UINT16_MAX) continue;  /* pool full */
                if (gpu) ai->morph_vb_gpu_mask |=  (1u << (unsigned)slot);
                else     ai->morph_vb_gpu_mask &= ~(1u << (unsigned)slot);
            }

            if (ai->morph_vb_gpu_mask & (1u << (unsigned)slot)) {
                /* One dispatch, no vertex traffic on this thread.  A failure
                 * here leaves the buffer holding the previous deform rather
                 * than uploading into a resource that cannot take it -- the
                 * CPU branch below is unreachable for this slot by
                 * construction, which is what the mask is for. */
                jce_morph_gpu_deform(md, sm, mesh_verts, ai->morph_weights,
                                     ai->morph_count, ai->morph_vb[slot]);
                continue;
            }

            /* Deform into a bgfx-owned transient buffer, then upload.  IMPORTANT:
             * jce_morph_apply writes ONLY the pos/normal fields — it does NOT
             * touch (or copy) the trailing uv/tangent/joints/weights bytes.  So
             * we first memcpy the FULL base vertex array through (preserving the
             * skinning attributes the GPU palette-skin reads), then run the
             * deform IN-PLACE over that copy (base==out aliasing is supported).
             * This guarantees the dynamic VB carries the SAME interleaved layout
             * as the static VB with only pos/normal rewritten. */
            uint32_t bytes = mesh_verts * stride;
            const bgfx_memory_t *mem = bgfx_alloc(bytes);
            if (!mem) continue;
            memcpy(mem->data, base, bytes);
            jce_morph_apply(md, ai->morph_weights, ai->morph_count,
                            mem->data, mem->data, mesh_verts, stride,
                            (int32_t)jce_skinned_mesh_pos_offset(sm),
                            (int32_t)jce_skinned_mesh_normal_offset(sm));
            bgfx_update_dynamic_vertex_buffer(ai->morph_vb[slot], 0, mem);
        }
    }

    /* Record the uploaded weight vector for the next frame's dirty compare. */
    memcpy(ai->morph_last_weights, ai->morph_weights,
           ai->morph_count * sizeof(float));
    ai->morph_last_count = (int)ai->morph_count;
}

/* Sample the active clip's morph-weight track into `out`.
 *
 * Returns the number of weights written, or 0 when there is no track for this
 * clip -- which is every model that has no blendshape animation, so the common
 * path costs one integer compare against num_morph_anims.
 *
 * MATCHED ON THE ANIMATION INDEX, which is the same index space `active_clip`
 * indexes: jce_gltf_loader.c stores the cgltf animation ordinal, and
 * jce_model_get_anim takes that ordinal.  Getting this wrong would sample a
 * DIFFERENT clip's weights and still animate, which is the failure mode worth
 * naming.
 *
 * ONE TRACK PER INSTANCE, and it is a real limit rather than an oversight.
 * The resolved weight vector (ai->morph_weights) is per-INSTANCE and every
 * morph-bearing primitive of that instance deforms with it, so a clip that
 * animates two different nodes' blendshapes cannot be honoured here without
 * per-node weights.  That case warns once rather than silently animating one
 * node and freezing the other -- the shape this tree keeps finding. */
uint32_t sr_sample_morph_track(SrAnimInstance *ai, float *out,
                                      uint32_t max_out)
{
    if (!ai || !ai->model || !ai->player || ai->active_clip < 0) return 0;

    const uint32_t n = jce_model_morph_anim_count(ai->model);
    if (n == 0) return 0;

    const JceMorphWeightTrack *track = NULL;
    uint32_t matches = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t anim_index = 0;
        const JceMorphWeightTrack *t =
            jce_model_morph_anim_track(ai->model, i, &anim_index, NULL);
        if (!t || anim_index != (uint32_t)ai->active_clip) continue;
        if (!track) track = t;
        matches++;
    }
    if (!track) return 0;

    if (matches > 1 && !ai->morph_multi_node_warned) {
        ai->morph_multi_node_warned = true;
        LOG_WARN(LOG_TAG,
                 "entity %u: clip %d animates blendshapes on %u nodes; this "
                 "renderer resolves ONE weight vector per instance, so only "
                 "the first is driven",
                 ai->entity, ai->active_clip, matches);
    }

    /* Clip-local seconds, which is what the track's timestamps are in. */
    return jce_morph_weight_track_sample(
        track, jce_anim_player_get_time(ai->player), out, max_out);
}
