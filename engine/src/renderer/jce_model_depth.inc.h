/* Model depth and motion owners; included once by jce_model.c. */
static void model_draw_shadow(const JceModel *model,
                              const JceRenderer *r, uint16_t view_id,
                              const jce_mat4 *transform,
                              const jce_mat4 *joint_matrices,
                              uint32_t num_joints,
                              JceModelMorphVbCb vb_cb, void *vb_user,
                              bool prepared_cpu_skin)
{
    if (!model || !r) return;

    const bool cpu_skin = jce_skinned_mesh_cpu_skin_required();
    ModelCpuSkin skin;
    skin.palette = joint_matrices;
    skin.count = num_joints;

    /* Depth-only mirror of jce_model_draw. MASK primitives bind their alpha
       source and UV transform; opaque programs keep the sample-free path.
       Skinned primitives reuse the color pass's world-space bone palette. */
    const JceShaderHandle opaque_skinned = jce_renderer_get_program_shadow_skinned(r);
    const JceShaderHandle opaque_static  = jce_renderer_get_program_shadow(r);

    jce_mat4 identity = jce_m4_identity();
    const jce_mat4 *root = transform ? transform : &identity;

    for (uint32_t n = 0; n < model->num_nodes; n++) {
        const JceModelNode *node = &model->nodes[n];
        jce_mat4 world = jce_m4_multiply(root, &node->local_transform);

        for (uint32_t p = 0; p < node->num_primitives; p++) {
            const JceModelPrimitive *prim = &node->primitives[p];
            const bool depth_skinned = prim->skinned_mesh &&
                jce_skinned_mesh_is_skinned(prim->skinned_mesh) && !cpu_skin;
            JceShaderHandle depth_program;
            if (!model_depth_select(model, prim,
                    depth_skinned ? JCE_PBR_DEPTH_SKINNED : JCE_PBR_DEPTH_STATIC,
                    depth_skinned ? opaque_skinned : opaque_static, &depth_program)) continue;

            if (prim->skinned_mesh) {
                const bool skinned = jce_skinned_mesh_is_skinned(prim->skinned_mesh);
                JceSkinVertexSource source = { NULL, UINT16_MAX };
                if (!skinned || !cpu_skin || prepared_cpu_skin)
                    source.dynamic_idx = morph_vb_for(vb_cb, vb_user, n, p);
                if (skinned) {
                    if (cpu_skin) {
                        bgfx_transient_vertex_buffer_t transient;
                        if (depth_program.idx != UINT16_MAX &&
                            model_prepare_cpu_skin(model, prim->skinned_mesh,
                                root, &world, &skin, prepared_cpu_skin && vb_cb,
                                &transient, &source))
                            jce_skinned_mesh_submit_shadow_vertices(
                                prim->skinned_mesh, r, view_id, depth_program, &source);
                        continue;
                    }
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
                    jce_skinned_mesh_submit_shadow_vertices(
                        prim->skinned_mesh, r, view_id, depth_program, &source);
                } else {
                    /* Static-PBR primitive (tangents, no joints): respect
                       joint-parent attachment, depth via the static program
                       (MASK additionally carries the original UV). */
                    jce_mat4 static_world = jce_model_static_node_world(
                        model, node, root, &world, joint_matrices, num_joints);
                    bgfx_set_transform(static_world.raw[0], 1);
                    /* In-asset auto-LOD: cast the SAME reduced silhouette the
                     * color pass drew so shadows match the rendered LOD.  Level
                     * 0 / no LOD => identical to the base shadow submit. */
                    if (source.dynamic_idx != UINT16_MAX)
                        jce_skinned_mesh_submit_shadow_vertices(
                            prim->skinned_mesh, r, view_id, depth_program, &source);
                    else if (s_draw_lod_level > 0)
                        jce_skinned_mesh_submit_shadow_lod(prim->skinned_mesh, r,
                                                           view_id, depth_program,
                                                           s_draw_lod_level - 1);
                    else
                        jce_skinned_mesh_submit_shadow(prim->skinned_mesh, r,
                                                       view_id, depth_program);
                }
            } else if (prim->static_mesh) {
                jce_mat4 static_world = jce_model_static_node_world(
                    model, node, root, &world, joint_matrices, num_joints);
                bgfx_set_transform(static_world.raw[0], 1);
                model_depth_static_mesh(prim->static_mesh, view_id, depth_program);
            }
        }
    }
}

void jce_model_draw_shadow(const JceModel *model,
                           const JceRenderer *r, uint16_t view_id,
                           const jce_mat4 *transform,
                           const jce_mat4 *joint_matrices,
                           uint32_t num_joints)
{
    model_draw_shadow(model, r, view_id, transform, joint_matrices, num_joints,
                      NULL, NULL, false);
}

void jce_model_draw_shadow_deformed(const JceModel *model,
                                    const JceRenderer *r, uint16_t view_id,
                                    const jce_mat4 *transform,
                                    const jce_mat4 *joint_matrices,
                                    uint32_t num_joints,
                                    JceModelMorphVbCb vb_cb, void *vb_user)
{
    if (!jce_skinned_mesh_cpu_skin_required()) {
        jce_model_draw_morphed_shadow(model, r, view_id, transform,
                                      joint_matrices, num_joints, vb_cb, vb_user);
        return;
    }
    model_draw_shadow(model, r, view_id, transform, joint_matrices, num_joints,
                      vb_cb, vb_user, true);
}

/* GPU-instanced depth-only mirror of jce_model_draw_shadow: rasterizes the same
 * static (non-skinned) primitives for `count` instances in one submit per
 * primitive via the shadow_inst program (vs_shadow_inst reads i_data0..3 as the
 * world matrix).  Skinned primitives are not instanceable (per-instance bone
 * palette) and are skipped here — callers must draw those per-entity.  No
 * material is bound (depth-only). */
static bool model_draw_shadow_instanced_cpu(const JceModel *model,
                                     const JceRenderer *r, uint16_t view_id,
                                     const jce_mat4 *roots, uint32_t count, bool explicit_lod)
{
    if (!model || !r || !roots || count == 0) return false;

    JceShaderHandle prog_inst = jce_renderer_get_program_shadow_inst(r);
    if (explicit_lod && prog_inst.idx == UINT16_MAX) return false;
    if (!explicit_lod && (count == 1 || prog_inst.idx == UINT16_MAX)) {
        for (uint32_t i = 0; i < count; i++)
            jce_model_draw_shadow(model, r, view_id, &roots[i], NULL, 0);
        return true;
    }
    const uint16_t stride = (uint16_t)sizeof(jce_mat4);   /* 64 bytes (4 vec4) */
    /* Same depth-only state as jce_mesh_submit_shadow (front-face cull). */
    const uint64_t shadow_state = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                                | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA;

    bool complete = true;
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
                JceInstanceUpload upload;
                if (!model_instance_begin(r, want, stride, &upload)) { complete = false; break; }
                uint32_t nb = upload.count;
                uint8_t *dst = (uint8_t *)upload.data;
                for (uint32_t i = 0; i < nb; i++) {
                    jce_mat4 world = jce_m4_multiply(&roots[start + i], &node_lt);
                    jce_mat4 sw = jce_model_static_node_world(model, node,
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
                    if (explicit_lod) {
                        /* Match resident scatter depth: zero is reduced IB[0],
                         * clamped independently for every primitive. Shadow
                         * triangle geometry is independent of color wireframe. */
                        jce_skinned_mesh_bind_shadow(sm);
                        const uint32_t levels = jce_skinned_mesh_lod_count(sm);
                        if (levels) {
                            const uint32_t level = s_draw_lod_level < levels
                                                 ? s_draw_lod_level : levels - 1u;
                            bgfx_index_buffer_handle_t ibh = {
                                (uint16_t)jce_skinned_mesh_lod_ibh(sm, level)};
                            if (ibh.idx != UINT16_MAX)
                                bgfx_set_index_buffer(ibh, 0, jce_skinned_mesh_lod_index_count(sm, level));
                        }
                    } else if (s_draw_lod_level > 0)
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
                bgfx_set_state(jce_pbr_cull_override(shadow_state, BGFX_STATE_CULL_MASK), 0);   /* depth-only (override color state) */
                JceShaderHandle depth_program;
                if (!model_depth_select(model, prim, JCE_PBR_DEPTH_INSTANCED,
                        prog_inst, &depth_program)) { complete = false; bgfx_discard(BGFX_DISCARD_ALL); break; }
                const bgfx_program_handle_t bgfx_prog = {depth_program.idx};
                if (!model_instance_bind(&upload, nb)) { complete = false; break; }
                bgfx_submit(view_id, bgfx_prog, 0, BGFX_DISCARD_ALL);

                start += nb;
            }
        }
    }
    return complete;
}

void jce_model_draw_shadow_instanced(const JceModel *model,
                                     const JceRenderer *r, uint16_t view_id,
                                     const jce_mat4 *roots, uint32_t count)
{
    (void)model_draw_shadow_instanced_cpu(model, r, view_id, roots, count, false);
}

bool jce_model_draw_shadow_instanced_cpu_lod(const JceModel *model,
                                             const JceRenderer *r, uint16_t view_id,
                                             const jce_mat4 *roots, uint32_t count,
                                             uint32_t level)
{
    const uint32_t saved = s_draw_lod_level;
    s_draw_lod_level = level;
    const bool complete = model_draw_shadow_instanced_cpu(model, r, view_id, roots, count, true);
    s_draw_lod_level = saved;
    return complete;
}

/* Depth-only sibling of jce_model_draw_bindpose_instanced: casts `count`
 * NON-animating (bind-pose) skinned instances of a model into a shadow view in
 * one instanced submit per skinned primitive, via the instanced SHADOW program
 * (vs_shadow_inst reads i_data0..3 as the per-instance world matrix) with
 * depth-only state and a MASK-only alpha bind. A bind-pose skinned mesh has its
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
                JceInstanceUpload upload;
                if (!model_instance_begin(r, want, stride, &upload)) break;
                uint32_t nb = upload.count;
                uint8_t *dst = (uint8_t *)upload.data;
                for (uint32_t i = 0; i < nb; i++)
                    memcpy(dst + (size_t)i * stride, worlds[start + i].raw[0], sizeof(jce_mat4));

                jce_skinned_mesh_bind_shadow(sm);   /* binds VB/tri-IB + depth state (no wireframe) */
                JceShaderHandle depth_program;
                if (!model_depth_select(model, prim, JCE_PBR_DEPTH_INSTANCED,
                        prog_inst, &depth_program)) { bgfx_discard(BGFX_DISCARD_ALL); break; }
                const bgfx_program_handle_t prog = {depth_program.idx};
                if (!model_instance_bind(&upload, nb)) break;
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
                JceInstanceUpload upload;
                if (!model_instance_begin(r, want, stride, &upload)) break;
                uint32_t nb = upload.count;
                uint8_t *dst = (uint8_t *)upload.data;
                for (uint32_t i = 0; i < nb; i++) {
                    uint8_t *slot = dst + (size_t)i * stride;
                    memcpy(slot, worlds[start + i].raw[0], sizeof(jce_mat4)); /* i_data0..3 */
                    float base4[4] = { (float)bases[start + i], 0.0f, 0.0f, 0.0f };
                    memcpy(slot + sizeof(jce_mat4), base4, sizeof(base4));     /* i_data4 */
                }

                jce_skinned_mesh_bind_shadow(sm);   /* binds VB/tri-IB + depth state */
                bgfx_set_texture(4, s_bones, bone_tex, UINT32_MAX);
                bgfx_set_uniform(u_params, params, 1);
                JceShaderHandle depth_program;
                if (!model_depth_select(model, prim, JCE_PBR_DEPTH_SKINNED_INSTANCED,
                        (JceShaderHandle){prog_idx}, &depth_program)) { bgfx_discard(BGFX_DISCARD_ALL); break; }
                const bgfx_program_handle_t prog = {depth_program.idx};
                if (!model_instance_bind(&upload, nb)) break;
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
                JceInstanceUpload upload;
                if (!model_instance_begin(r, want, stride, &upload)) break;
                uint32_t nb = upload.count;
                uint8_t *dst = (uint8_t *)upload.data;
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
                JceShaderHandle depth_program;
                if (!model_depth_select(model, prim, JCE_PBR_DEPTH_VELOCITY_SKINNED_INSTANCED,
                        (JceShaderHandle){prog_idx}, &depth_program)) { bgfx_discard(BGFX_DISCARD_ALL); break; }
                const bgfx_program_handle_t prog = {depth_program.idx};
                if (!model_instance_bind(&upload, nb)) break;
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
            const bool depth_skinned = prim->skinned_mesh &&
                jce_skinned_mesh_is_skinned(prim->skinned_mesh) && have_skinned;
            const bool motion = ctx->u_cur_vp_idx != UINT16_MAX;
            const JcePbrDepthKind kind = motion
                ? (depth_skinned ? JCE_PBR_DEPTH_VELOCITY_SKINNED : JCE_PBR_DEPTH_VELOCITY_STATIC)
                : (depth_skinned ? JCE_PBR_DEPTH_GBUFFER_SKINNED : JCE_PBR_DEPTH_GBUFFER_STATIC);
            JceShaderHandle depth_program;
            if (!model_depth_select(model, prim, kind,
                    depth_skinned ? ctx->skinned_program : ctx->static_program,
                    &depth_program)) continue;

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
                    bgfx_program_handle_t prog = { depth_program.idx };
                    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
                } else {
                    /* Static-PBR primitive (no joints): static velocity program. */
                    jce_mat4 static_world = jce_model_static_node_world(
                        model, node, root, &world, joint_matrices, num_joints);
                    jce_mat4 static_prev = jce_model_static_node_world(
                        model, node, prev_root, &prev_world,
                        prev_joint_matrices ? prev_joint_matrices : joint_matrices,
                        prev_joint_matrices ? num_prev_joints : num_joints);
                    bgfx_set_transform(static_world.raw[0], 1);
                    if (u_prevmodel.idx != UINT16_MAX)
                        bgfx_set_uniform(u_prevmodel, static_prev.raw[0], 1);
                    jce_skinned_mesh_submit(prim->skinned_mesh, r, view_id);
                    bgfx_program_handle_t prog = { depth_program.idx };
                    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
                }
            } else if (prim->static_mesh) {
                jce_mat4 static_world = jce_model_static_node_world(
                    model, node, root, &world, joint_matrices, num_joints);
                jce_mat4 static_prev = jce_model_static_node_world(
                    model, node, prev_root, &prev_world,
                    prev_joint_matrices ? prev_joint_matrices : joint_matrices,
                    prev_joint_matrices ? num_prev_joints : num_joints);
                bgfx_set_transform(static_world.raw[0], 1);
                if (u_prevmodel.idx != UINT16_MAX)
                    bgfx_set_uniform(u_prevmodel, static_prev.raw[0], 1);
                jce_mesh_submit_pbr_with_program(prim->static_mesh, r, view_id,
                                                 depth_program);
            }
        }
    }
}
