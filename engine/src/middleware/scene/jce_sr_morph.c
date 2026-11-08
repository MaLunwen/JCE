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
#include "renderer/jce_skinned_mesh_deform_internal.h"
#include "renderer/jce_model_deform_internal.h"

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
static uint16_t sr_morph_vb_cb(void *user, uint32_t node, uint32_t prim)
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

/* Compatibility keeps one posed buffer per primitive and instance. The
 * immutable model still owns the source geometry, layout and index buffers. */
typedef struct {
    const JceSkinnedMesh *mesh;
    const JceMorphData *morph;
    uint32_t node;
    uint32_t primitive;
    bgfx_dynamic_vertex_buffer_handle_t buffer;
    bool ready;
} SrCpuDeformPrimitive;

struct SrCpuDeformState {
    uint32_t count;
    bool warned;
    SrCpuDeformPrimitive primitives[];
};

static const JceSkinnedMesh *cpu_deform_mesh(const JceModel *model, uint32_t n,
                                            uint32_t p, const JceMorphData **md)
{
    const JceSkinnedMesh *mesh = jce_model_prim_skinned_mesh(model, n, p);
    if (!mesh)
        return NULL;
    *md = jce_model_prim_morph(model, n, p);
    if (*md && jce_morph_vertex_count(*md) != jce_skinned_mesh_vertex_count(mesh))
        *md = NULL;
    return *md || jce_skinned_mesh_is_skinned(mesh) ? mesh : NULL;
}

static SrCpuDeformState *cpu_deform_create(const JceModel *model)
{
    const uint32_t nodes = jce_model_node_count(model);
    const size_t capacity = (SIZE_MAX - sizeof(SrCpuDeformState)) /
                            sizeof(SrCpuDeformPrimitive);
    uint32_t count = 0;
    for (uint32_t n = 0; n < nodes; ++n) {
        const uint32_t primitives = jce_model_node_prim_count(model, n);
        for (uint32_t p = 0; p < primitives; ++p) {
            const JceMorphData *md;
            if (cpu_deform_mesh(model, n, p, &md)) {
                if (count == UINT32_MAX || (size_t)count == capacity)
                    return NULL;
                ++count;
            }
        }
    }
    SrCpuDeformState *state = JCE_CALLOC(
        1, sizeof(*state) + count * sizeof(*state->primitives));
    if (!state)
        return NULL;
    for (uint32_t n = 0; n < nodes; ++n) {
        const uint32_t primitives = jce_model_node_prim_count(model, n);
        for (uint32_t p = 0; p < primitives; ++p) {
            const JceMorphData *md;
            const JceSkinnedMesh *mesh = cpu_deform_mesh(model, n, p, &md);
            if (!mesh)
                continue;
            SrCpuDeformPrimitive *slot = &state->primitives[state->count++];
            *slot = (SrCpuDeformPrimitive){mesh, md, n, p, {UINT16_MAX}, false};
        }
    }
    return state;
}

void sr_free_cpu_deform(SrAnimInstance *ai)
{
    SrCpuDeformState *state = ai->cpu_deform;
    if (!state)
        return;
    ai->cpu_deform = NULL;
    for (uint32_t i = 0; i < state->count; ++i) {
        if (BGFX_HANDLE_IS_VALID(state->primitives[i].buffer))
            bgfx_destroy_dynamic_vertex_buffer(state->primitives[i].buffer);
    }
    JCE_FREE(state);
}

static uint16_t cpu_deform_vb_cb(void *user, uint32_t node, uint32_t primitive)
{
    const SrAnimInstance *ai = user;
    const SrCpuDeformState *state = ai->cpu_deform;
    uint32_t left = 0, right = state->count;
    while (left < right) {
        const uint32_t mid = left + (right - left) / 2u;
        const SrCpuDeformPrimitive *slot = &state->primitives[mid];
        if (slot->node == node && slot->primitive == primitive)
            return slot->ready ? slot->buffer.idx : UINT16_MAX;
        if (slot->node < node || (slot->node == node && slot->primitive < primitive))
            left = mid + 1u;
        else
            right = mid;
    }
    return UINT16_MAX;
}

JceModelMorphVbCb sr_anim_geometry_vb_cb(const SrAnimInstance *ai)
{
    if (!ai)
        return NULL;
    if (ai->cpu_deform && ai->cpu_deform->count > 0)
        return cpu_deform_vb_cb;
    return ai->morph_vb_count > 0 ? sr_morph_vb_cb : NULL;
}

static bool cpu_deform_upload(SrCpuDeformPrimitive *slot, const SrAnimInstance *ai,
                              const jce_mat4 *palette, uint32_t joints)
{
    const JceSkinnedMesh *mesh = slot->mesh;
    const bool skin = jce_skinned_mesh_is_skinned(mesh);
    const bool morph = slot->morph && ai->morph_count > 0;
    slot->ready = false;
    if (!skin && !morph)
        return true;

    const uint32_t vertices = jce_skinned_mesh_vertex_count(mesh);
    const uint32_t stride = jce_skinned_mesh_stride(mesh);
    const void *base = jce_skinned_mesh_base_verts(mesh);
    const bgfx_vertex_layout_t *layout = jce_skinned_mesh_layout(mesh);
    if (!base || !layout || !vertices || !stride || vertices > UINT32_MAX / stride)
        return false;
    if (!BGFX_HANDLE_IS_VALID(slot->buffer)) {
        slot->buffer = bgfx_create_dynamic_vertex_buffer(
            vertices, layout, BGFX_BUFFER_NONE);
        if (!BGFX_HANDLE_IS_VALID(slot->buffer))
            return false;
    }

    const uint32_t bytes = vertices * stride;
    const bgfx_memory_t *mem = bgfx_alloc(bytes);
    if (!mem)
        return false;
    memcpy(mem->data, base, bytes);
    if (morph)
        jce_morph_apply(slot->morph, ai->morph_weights, ai->morph_count,
                        mem->data, mem->data, vertices, stride,
                        (int32_t)jce_skinned_mesh_pos_offset(mesh),
                        (int32_t)jce_skinned_mesh_normal_offset(mesh));
    if (skin)
        jce_skin_deform_vertices((JceSkinnedVertex *)mem->data, vertices,
                                 palette, joints);
    bgfx_update_dynamic_vertex_buffer(slot->buffer, 0, mem);
    slot->ready = true;
    return true;
}

void sr_deform_cpu_skin_prims(SrAnimInstance *ai)
{
    if (!ai || !ai->model)
        return;
    if (!ai->cpu_deform)
        ai->cpu_deform = cpu_deform_create(ai->model);
    SrCpuDeformState *state = ai->cpu_deform;
    if (!state || !state->count)
        return;

    uint32_t joints = ai->skin_palette_count;
    if (joints > JCE_MAX_BONES)
        joints = JCE_MAX_BONES;
    const bool same_pose = joints == ai->prev_skin_palette_count &&
        (!joints || memcmp(ai->skin_palette, ai->prev_skin_palette,
                           joints * sizeof(jce_mat4)) == 0);
    const size_t weight_bytes = ai->morph_count * sizeof(float);
    if (same_pose && ai->morph_last_count == (int)ai->morph_count &&
        memcmp(ai->morph_last_weights, ai->morph_weights, weight_bytes) == 0)
        return;

    jce_mat4 bind[JCE_MAX_BONES];
    const jce_mat4 *palette = ai->skin_palette;
    const JceSkeleton *skeleton = jce_model_get_skeleton(ai->model);
    if (!joints && skeleton) {
        joints = jce_skeleton_joint_count(skeleton);
        if (joints > JCE_MAX_BONES)
            joints = JCE_MAX_BONES;
        jce_skeleton_evaluate(skeleton, NULL, bind, joints);
        palette = bind;
    }

    bool complete = true;
    for (uint32_t i = 0; i < state->count; ++i) {
        if (!cpu_deform_upload(&state->primitives[i], ai, palette, joints))
            complete = false;
    }
    if (!complete && !state->warned) {
        state->warned = true;
        LOG_WARN(LOG_TAG, "compatibility skin buffer unavailable for entity %llu",
                 (unsigned long long)ai->entity);
    }
    memcpy(ai->morph_last_weights, ai->morph_weights, weight_bytes);
    ai->morph_last_count = complete ? (int)ai->morph_count : -1;
}

void jce_scene_renderer_submit_model_wireframe(
    JceSceneRenderer *sr, JceScene *scene, uint64_t entity,
    const JceModel *model, uint16_t view_id, const jce_mat4 *world)
{
    if (!sr || !scene || !model)
        return;
    SrAnimInstance *ai = sr->anim_storage_scene == scene
        ? sr_find_anim_instance(sr, entity) : NULL;
    if (ai && ai->model != model)
        ai = NULL;
    const uint32_t joints = ai ? ai->skin_palette_count : 0;
    jce_model_submit_view_wireframe(
        model, sr->renderer, view_id, world, joints ? ai->skin_palette : NULL,
        joints, sr_anim_geometry_vb_cb(ai), ai, true, NULL, NULL);
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
