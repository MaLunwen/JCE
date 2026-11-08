/* A private camera-space hair target for the first eligible direct toon draw
 * in each standard scene viewport. */
#include "jce_sr_internal.h"
#include "renderer/jce_model_toon_hair_internal.h"
#include "renderer/jce_render_encoder.h"
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_shaders.h>

enum { TOON_HAIR_TARGET_MAX = 512, TOON_HAIR_VIEWPORTS = 3 };

typedef struct {
    bgfx_frame_buffer_handle_t framebuffer;
    bgfx_texture_handle_t eye_depth, raster_depth;
    uint16_t width, height;
} SrToonHairTarget;

struct SrToonHair {
    SrToonHairTarget targets[TOON_HAIR_VIEWPORTS];
    bgfx_program_handle_t program;
    bgfx_uniform_handle_t view_proj_uniform;
    jce_mat4 view, proj, view_proj;
    uint16_t base, width, height;
    int active_target;
    bool shader_attempted;
    bool ordered;
    bool used;
    bool binding;
};

static int toon_hair_viewport(uint16_t base)
{
    switch (base) {
    case JCE_VIEW_EDITOR_SCENE: return 0;
    case JCE_VIEW_EDITOR_GAME: return 1;
    case JCE_VIEW_RUNTIME_GAME: return 2;
    default: return -1;
    }
}

static void toon_hair_target_destroy(SrToonHairTarget *target)
{
    if (BGFX_HANDLE_IS_VALID(target->framebuffer))
        bgfx_destroy_frame_buffer(target->framebuffer);
    if (BGFX_HANDLE_IS_VALID(target->eye_depth))
        bgfx_destroy_texture(target->eye_depth);
    if (BGFX_HANDLE_IS_VALID(target->raster_depth))
        bgfx_destroy_texture(target->raster_depth);
}

static bool toon_hair_target_ensure(SrToonHairTarget *target,
                                   uint16_t width, uint16_t height)
{
    if (target->width == width && target->height == height)
        return BGFX_HANDLE_IS_VALID(target->framebuffer);

    const uint64_t color_flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP |
        BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
    const uint64_t depth_flags = BGFX_TEXTURE_RT;
    if (!bgfx_is_texture_valid(0, false, 1, BGFX_TEXTURE_FORMAT_R32F, color_flags) ||
        !bgfx_is_texture_valid(0, false, 1, BGFX_TEXTURE_FORMAT_D24S8, depth_flags))
        return false;

    bgfx_texture_handle_t attachments[2];
    attachments[0] = bgfx_create_texture_2d(width, height, false, 1,
        BGFX_TEXTURE_FORMAT_R32F, color_flags, NULL, 0);
    if (!BGFX_HANDLE_IS_VALID(attachments[0])) return false;
    attachments[1] = bgfx_create_texture_2d(width, height, false, 1,
        BGFX_TEXTURE_FORMAT_D24S8, depth_flags, NULL, 0);
    if (!BGFX_HANDLE_IS_VALID(attachments[1])) {
        bgfx_destroy_texture(attachments[0]);
        return false;
    }

    bgfx_frame_buffer_handle_t fb = bgfx_create_frame_buffer_from_handles(2, attachments, false);
    if (!BGFX_HANDLE_IS_VALID(fb)) {
        bgfx_destroy_texture(attachments[1]);
        bgfx_destroy_texture(attachments[0]);
        return false;
    }
    toon_hair_target_destroy(target);
    *target = (SrToonHairTarget){fb, attachments[0], attachments[1], width, height};
    return true;
}

bool sr_toon_hair_prepare(JceSceneRenderer *sr, const JceCamera *camera,
                          uint16_t base, const JceSceneRenderConfig *cfg)
{
    SrToonHair *hair = sr->toon_hair;
    if (hair) {
        hair->ordered = false;
        if (hair->shader_attempted && (!BGFX_HANDLE_IS_VALID(hair->program) ||
            !BGFX_HANDLE_IS_VALID(hair->view_proj_uniform))) return false;
    }
    const int index = toon_hair_viewport(base);
    if (index < 0 || !camera || cfg->reduced_pass || !cfg->draw_opaque ||
        cfg->view_mode != JCE_SCENE_VIEW_SHADED ||
        !cfg->viewport_width || !cfg->viewport_height ||
        !jce_shader_valid(jce_renderer_get_program_pbr_skinned_toon(sr->renderer)))
        return false;

    JceRenderPipelineDesc pipeline;
    jce_render_pipeline_get(&pipeline);
    if (pipeline.post_quality < JCE_RP_QUALITY_HIGH) return false;

    if (!hair) {
        hair = JCE_CALLOC(1, sizeof(*hair));
        if (!hair) return false;
        for (unsigned i = 0; i < TOON_HAIR_VIEWPORTS; ++i) {
            SrToonHairTarget *target = &hair->targets[i];
            target->framebuffer.idx = target->eye_depth.idx = target->raster_depth.idx = UINT16_MAX;
        }
        hair->program.idx = UINT16_MAX;
        hair->view_proj_uniform.idx = UINT16_MAX;
        sr->toon_hair = hair;
    }

    const uint32_t width = cfg->viewport_width;
    const uint32_t height = cfg->viewport_height;
    const uint32_t longest = width > height ? width : height;
    const uint32_t limit = longest > TOON_HAIR_TARGET_MAX ? TOON_HAIR_TARGET_MAX : longest;
    const uint32_t rt_width = (uint32_t)((uint64_t)width * limit / longest);
    const uint32_t rt_height = (uint32_t)((uint64_t)height * limit / longest);
    hair->width = (uint16_t)(rt_width ? rt_width : 1u);
    hair->height = (uint16_t)(rt_height ? rt_height : 1u);
    hair->view = jce_camera_view(camera);
    hair->proj = jce_camera_proj(camera, (float)width / (float)height, sr->homogeneous_depth);
    hair->view_proj = jce_m4_multiply(&hair->proj, &hair->view);
    hair->base = base;
    hair->active_target = index;
    hair->used = false;
    return true;
}

void sr_toon_hair_order_ready(JceSceneRenderer *sr, bool ready)
{
    if (sr->toon_hair) sr->toon_hair->ordered = ready;
}

static bool toon_hair_begin(JceSceneRenderer *sr, uint16_t base,
                            const JceModel *model, JceShaderHandle toon_program,
                            JceModelToonHairPass *pass)
{
    SrToonHair *hair = sr->toon_hair;
    if (!hair || !hair->ordered || hair->used || hair->base != base ||
        !jce_shader_valid(toon_program) || jce_renderer_get_wireframe(sr->renderer) ||
        !jce_model_has_toon_hair(model))
        return false;

    if (!hair->shader_attempted) {
        hair->shader_attempted = true;
        hair->program.idx = shader_load_program_named(sr->pak, "pbr_skinned", "toon_hair_depth").idx;
        if (BGFX_HANDLE_IS_VALID(hair->program))
            hair->view_proj_uniform = bgfx_create_uniform("u_toonHairViewProj",
                                                           BGFX_UNIFORM_TYPE_MAT4, 1);
    }
    SrToonHairTarget *target = &hair->targets[hair->active_target];
    if (!BGFX_HANDLE_IS_VALID(hair->program) || !BGFX_HANDLE_IS_VALID(hair->view_proj_uniform) ||
        !toon_hair_target_ensure(target, hair->width, hair->height)) return false;

    const uint16_t view = (uint16_t)(base + JCE_VIEW_SR_TOON_HAIR_OFFSET);
    bgfx_set_view_name(view, "Scene/ToonHairDepth", INT32_MAX);
    bgfx_set_view_mode(view, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_set_view_rect(view, 0, 0, target->width, target->height);
    bgfx_set_view_frame_buffer(view, target->framebuffer);
    bgfx_set_view_transform(view, hair->view.raw[0], hair->proj.raw[0]);
    bgfx_set_view_clear(view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
    bgfx_touch(view);
    *pass = (JceModelToonHairPass){view, hair->program.idx, target->eye_depth.idx};
    hair->used = true;
    return true;
}

void sr_toon_hair_draw(JceSceneRenderer *sr, const JceModel *model, uint16_t view_id,
                       const jce_mat4 *root, const jce_mat4 *palette, uint32_t count,
                       JceShaderHandle toon_program)
{
    JceModelToonHairPass pass;
    const bool active = toon_hair_begin(sr, view_id, model, toon_program, &pass);
    if (active) {
        sr->toon_hair->binding = true;
        jce_model_set_toon_hair_pass(&pass);
    }
    jce_model_draw_program(model, sr->renderer, view_id, root, palette, count, toon_program);
    if (active) {
        jce_model_set_toon_hair_pass(NULL);
        sr->toon_hair->binding = false;
    }
}

/* Set toon uniforms before each toon-character primitive submit (mirrors the
 * fwdplus cluster-bind pre-submit hook: bgfx clears uniform state per submit). */
void sr_model_toon_presubmit_cb(void *user, uint16_t view_id)
{
    JceSceneRenderer *sr = (JceSceneRenderer *)user;
    if (!sr) return;
    sr_inline_bind_pbr_global(sr, NULL, view_id, NULL, NULL,
                             sr->model_receiver_layer);
    if (sr->toon_hair && sr->toon_hair->binding)
        jce_enc_set_uniform(sr->toon_hair->view_proj_uniform, sr->toon_hair->view_proj.raw[0], 1);
    bgfx_set_uniform(sr->u_toon_params,    sr->toon_params_frame,    1);
    bgfx_set_uniform(sr->u_toon_rim_color, sr->toon_rim_color_frame, 1);
}


void sr_toon_hair_destroy(JceSceneRenderer *sr)
{
    /* Toon character uniforms (stylized-slice §5.6). */
    if (BGFX_HANDLE_IS_VALID(sr->u_toon_params))    bgfx_destroy_uniform(sr->u_toon_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_toon_rim_color)) bgfx_destroy_uniform(sr->u_toon_rim_color);
    if (BGFX_HANDLE_IS_VALID(sr->u_outline_params)) bgfx_destroy_uniform(sr->u_outline_params);
    if (BGFX_HANDLE_IS_VALID(sr->u_outline_color))  bgfx_destroy_uniform(sr->u_outline_color);
    SrToonHair *hair = sr->toon_hair;
    if (!hair) return;
    for (unsigned i = 0; i < TOON_HAIR_VIEWPORTS; ++i)
        toon_hair_target_destroy(&hair->targets[i]);
    if (BGFX_HANDLE_IS_VALID(hair->view_proj_uniform))
        bgfx_destroy_uniform(hair->view_proj_uniform);
    if (BGFX_HANDLE_IS_VALID(hair->program))
        jce_shader_program_destroy((JceShaderHandle){hair->program.idx});
    JCE_FREE(hair);
    sr->toon_hair = NULL;
}
