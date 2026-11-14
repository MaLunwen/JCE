/* Effective imported material for every depth primitive, matching colour. */
static const JcePbrMaterial *model_depth_material(const JceModel *model,
                                                 const JceModelPrimitive *prim)
{
    if (s_material_override) return s_material_override;
    return model->materials && prim->material_index < model->num_materials
        ? &model->materials[prim->material_index] : NULL;
}

bool jce_model_depth_masked(const JceModel *model)
{
    if (s_material_override) return s_material_override->alpha_mode == JCE_ALPHA_MASK;
    if (!model || !model->materials) return false;
    for (uint32_t i = 0; i < model->num_materials; ++i)
        if (model->materials[i].alpha_mode == JCE_ALPHA_MASK) return true;
    return false;
}

static bool model_depth_select(const JceModel *model, const JceModelPrimitive *prim,
    JcePbrDepthKind kind, JceShaderHandle opaque, JceShaderHandle *program)
{
    const JcePbrMaterial *mat = model_depth_material(model, prim);
    if (!mat || mat->alpha_mode != JCE_ALPHA_MASK) {
        *program = opaque;
        return jce_shader_valid(opaque);
    }
    *program = jce_pbr_depth_program(kind);
    return jce_shader_valid(*program) && jce_pbr_material_bind_depth(mat);
}

static void model_depth_static_mesh(const JceMesh *mesh,
    uint16_t view_id, JceShaderHandle program)
{
    const bgfx_vertex_buffer_handle_t vertex = {(uint16_t)jce_mesh_get_vbh(mesh)};
    const bgfx_index_buffer_handle_t index = {(uint16_t)jce_mesh_get_ibh(mesh)};
    jce_enc_set_vertex_buffer(0, vertex, 0, jce_mesh_vertex_count(mesh));
    if (index.idx != UINT16_MAX)
        jce_enc_set_index_buffer(index, 0, jce_mesh_index_count(mesh));
    jce_enc_set_state(jce_pbr_cull_override(BGFX_STATE_WRITE_Z |
        BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA,
        BGFX_STATE_CULL_MASK), 0);
    const bgfx_program_handle_t handle = {program.idx};
    jce_enc_submit(view_id, handle, 0, BGFX_DISCARD_ALL);
}
