#include <jce/renderer/jce_fullscreen_effect.h>

#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_shaders.h>

#include <bgfx/c99/bgfx.h>

#include <math.h>
#include <string.h>

typedef struct JceFullscreenVertex {
    float x, y, z;
    float u, v;
} JceFullscreenVertex;

static const JceFullscreenVertex s_quad_vertices[4] = {
    { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
    {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
    { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
    {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f }
};

static const uint16_t s_quad_indices[6] = { 0, 2, 1, 1, 2, 3 };

/* User sampler stages: 3, 4, 6, 7 -- NOT `3 + i`.  Stage 5 is skipped because
 * sr_bind_frame_shadow_state() binds s_shadowMap there for whatever draw is in
 * flight; a user texture landing on 5 wins or loses against the shadow map
 * depending on which bind ran last.  Neither outcome errors and neither reads
 * as a slot conflict -- it reads as a surface shaded oddly, which is why this
 * is a table and not arithmetic that quietly steps onto the reserved stage.
 *
 * jce_fullscreen_effect.sh declares these SAME four stages.  The two are one
 * fact written twice; change them together or not at all. */
static const uint8_t s_user_stage[] = { 3, 4, 6, 7 };

/* Deliberately NOT `s_user_stage[JCE_FULLSCREEN_EFFECT_MAX_TEXTURES]`: sized by
 * its initialiser, the count below is the number of stages actually written, so
 * the check has something to disagree with.  Sized by the macro it would always
 * hold -- a fifth texture would silently get a zero-filled fifth element, i.e.
 * stage 0, which is s_jceSceneColor.  (This assertion was written the tautologi-
 * cal way first; raising MAX_TEXTURES to 5 in a probe still compiled clean,
 * which is how the empty form was caught.)
 * C99 has no _Static_assert; a negative array bound is the portable form, and
 * it names the type in the error. */
typedef char jce_fullscreen_user_stage_table_covers_every_texture[
    (sizeof(s_user_stage) / sizeof(s_user_stage[0])
     == JCE_FULLSCREEN_EFFECT_MAX_TEXTURES) ? 1 : -1];

struct JceFullscreenEffectPipeline {
    jce_allocator_t allocator;
    const JcePakArchive *project_pak;

    bgfx_vertex_layout_t quad_layout;
    bgfx_vertex_buffer_handle_t quad_vb;
    bgfx_index_buffer_handle_t quad_ib;
    bgfx_program_handle_t program;
    char loaded_shader[JCE_FULLSCREEN_EFFECT_PATH_MAX];
    char shader_dev_dir[1024];

    bgfx_texture_handle_t output_texture[2];
    bgfx_frame_buffer_handle_t output_target[2];
    uint32_t target_width;
    uint32_t target_height;
    uint32_t target_format;
    uint8_t write_index;

    bgfx_texture_handle_t fallback_texture;

    bgfx_uniform_handle_t s_scene_color;
    bgfx_uniform_handle_t s_scene_depth;
    bgfx_uniform_handle_t s_history;
    bgfx_uniform_handle_t s_user[JCE_FULLSCREEN_EFFECT_MAX_TEXTURES];
    bgfx_uniform_handle_t u_view_proj;
    bgfx_uniform_handle_t u_inv_view_proj;
    bgfx_uniform_handle_t u_prev_view_proj;
    bgfx_uniform_handle_t u_effect_world;
    bgfx_uniform_handle_t u_effect_world_inv;
    bgfx_uniform_handle_t u_camera_position;
    bgfx_uniform_handle_t u_camera_basis;
    bgfx_uniform_handle_t u_camera_projection;
    bgfx_uniform_handle_t u_viewport;
    bgfx_uniform_handle_t u_output_viewport;
    bgfx_uniform_handle_t u_time_frame;
    bgfx_uniform_handle_t u_params;

    uint64_t history_key;
    bool history_valid;
    JceFullscreenEffectStatus status;
};

static void copy_bounded(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    if (!dst || dst_size == 0) return;
    if (src) {
        while (i + 1 < dst_size && src[i]) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static bool shader_name_valid(const char *name)
{
    size_t i;
    if (!name || !name[0]) return false;
    for (i = 0; i < JCE_FULLSCREEN_EFFECT_PATH_MAX && name[i]; i++) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return i > 0 && i < JCE_FULLSCREEN_EFFECT_PATH_MAX;
}

static bool color_format_valid(uint32_t format)
{
    switch ((JceRenderFormat)format) {
    case JCE_RENDER_FORMAT_RGBA8:
    case JCE_RENDER_FORMAT_RGBA16F:
    case JCE_RENDER_FORMAT_R32F:
    case JCE_RENDER_FORMAT_R16F:
    case JCE_RENDER_FORMAT_RG16F:
    case JCE_RENDER_FORMAT_RG32F:
    case JCE_RENDER_FORMAT_RGBA32F:
        return true;
    default:
        return false;
    }
}

static JceSamplerDesc default_sampler(void)
{
    JceSamplerDesc sampler;
    memset(&sampler, 0, sizeof(sampler));
    sampler.struct_size = sizeof(sampler);
    sampler.address_u = JCE_SAMPLER_ADDRESS_CLAMP;
    sampler.address_v = JCE_SAMPLER_ADDRESS_CLAMP;
    sampler.filter_min = JCE_SAMPLER_FILTER_LINEAR;
    sampler.filter_mag = JCE_SAMPLER_FILTER_LINEAR;
    sampler.filter_mip = JCE_SAMPLER_FILTER_LINEAR;
    return sampler;
}

JceFullscreenEffectPassDesc jce_fullscreen_effect_pass_desc_default(void)
{
    JceFullscreenEffectPassDesc desc;
    uint32_t i;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.version = JCE_FULLSCREEN_EFFECT_ABI_VERSION;
    desc.insertion = JCE_FULLSCREEN_EFFECT_HDR_BEFORE_POSTFX;
    desc.blend = JCE_FULLSCREEN_EFFECT_REPLACE;
    desc.output_format = JCE_RENDER_FORMAT_RGBA16F;
    desc.resolution_scale = 1.0f;
    for (i = 0; i < JCE_FULLSCREEN_EFFECT_MAX_TEXTURES; i++) {
        desc.textures[i] = JCE_INVALID_TEXTURE;
        desc.samplers[i] = default_sampler();
    }
    return desc;
}

JceFullscreenEffectFrameDesc jce_fullscreen_effect_frame_desc_default(void)
{
    JceFullscreenEffectFrameDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.scene_color = JCE_INVALID_TEXTURE;
    desc.scene_depth = JCE_INVALID_TEXTURE;
    desc.view_proj = jce_m4_identity();
    desc.inv_view_proj = jce_m4_identity();
    desc.prev_view_proj = jce_m4_identity();
    desc.effect_world = jce_m4_identity();
    desc.effect_world_inv = jce_m4_identity();
    desc.camera_right = jce_v3(1.0f, 0.0f, 0.0f);
    desc.camera_up = jce_v3(0.0f, 1.0f, 0.0f);
    desc.camera_forward = jce_v3(0.0f, 0.0f, -1.0f);
    desc.aspect_ratio = 1.0f;
    return desc;
}

bool jce_fullscreen_effect_sanitize(JceFullscreenEffectPassDesc *desc)
{
    uint32_t i, j;
    if (!desc || desc->struct_size != sizeof(*desc) ||
        desc->version != JCE_FULLSCREEN_EFFECT_ABI_VERSION)
        return false;

    desc->shader[JCE_FULLSCREEN_EFFECT_PATH_MAX - 1] = '\0';
    if (desc->enabled && !shader_name_valid(desc->shader)) return false;
    if (desc->texture_count > JCE_FULLSCREEN_EFFECT_MAX_TEXTURES)
        desc->texture_count = JCE_FULLSCREEN_EFFECT_MAX_TEXTURES;
    if (desc->insertion > JCE_FULLSCREEN_EFFECT_LDR_AFTER_POSTFX)
        desc->insertion = JCE_FULLSCREEN_EFFECT_HDR_BEFORE_POSTFX;
    if (desc->blend > JCE_FULLSCREEN_EFFECT_ALPHA)
        desc->blend = JCE_FULLSCREEN_EFFECT_REPLACE;
    if (!color_format_valid(desc->output_format))
        desc->output_format = JCE_RENDER_FORMAT_RGBA16F;
    if (!isfinite(desc->resolution_scale)) desc->resolution_scale = 1.0f;
    if (desc->resolution_scale < 0.0625f) desc->resolution_scale = 0.0625f;
    if (desc->resolution_scale > 1.0f) desc->resolution_scale = 1.0f;

    for (i = 0; i < JCE_FULLSCREEN_EFFECT_MAX_TEXTURES; i++) {
        JceSamplerDesc *s = &desc->samplers[i];
        if (s->struct_size != sizeof(*s)) *s = default_sampler();
        if (s->address_u > JCE_SAMPLER_ADDRESS_MIRROR)
            s->address_u = JCE_SAMPLER_ADDRESS_CLAMP;
        if (s->address_v > JCE_SAMPLER_ADDRESS_MIRROR)
            s->address_v = JCE_SAMPLER_ADDRESS_CLAMP;
        if (s->filter_min > JCE_SAMPLER_FILTER_LINEAR)
            s->filter_min = JCE_SAMPLER_FILTER_LINEAR;
        if (s->filter_mag > JCE_SAMPLER_FILTER_LINEAR)
            s->filter_mag = JCE_SAMPLER_FILTER_LINEAR;
        if (s->filter_mip > JCE_SAMPLER_FILTER_LINEAR)
            s->filter_mip = JCE_SAMPLER_FILTER_LINEAR;
    }
    for (i = 0; i < JCE_FULLSCREEN_EFFECT_MAX_PARAMS; i++) {
        for (j = 0; j < 4; j++) {
            if (!isfinite(desc->params[i][j])) desc->params[i][j] = 0.0f;
        }
    }
    return true;
}

void jce_fullscreen_effect_target_extent(float resolution_scale,
                                         uint32_t input_width,
                                         uint32_t input_height,
                                         uint32_t *out_width,
                                         uint32_t *out_height)
{
    double width, height;
    if (!isfinite(resolution_scale)) resolution_scale = 1.0f;
    if (resolution_scale < 0.0625f) resolution_scale = 0.0625f;
    if (resolution_scale > 1.0f) resolution_scale = 1.0f;
    if (input_width == 0) input_width = 1;
    if (input_height == 0) input_height = 1;
    width = floor((double)input_width * resolution_scale + 0.5);
    height = floor((double)input_height * resolution_scale + 0.5);
    if (width < 1.0) width = 1.0;
    if (height < 1.0) height = 1.0;
    if (width > 8192.0) width = 8192.0;
    if (height > 8192.0) height = 8192.0;
    if (out_width) *out_width = (uint32_t)width;
    if (out_height) *out_height = (uint32_t)height;
}

int jce_fullscreen_effect_sort_key_compare(
    const JceFullscreenEffectSortKey *a,
    const JceFullscreenEffectSortKey *b)
{
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    if (a->insertion != b->insertion)
        return a->insertion < b->insertion ? -1 : 1;
    if (a->order != b->order) return a->order < b->order ? -1 : 1;
    if (a->entity_id != b->entity_id) return a->entity_id < b->entity_id ? -1 : 1;
    return 0;
}

bool jce_fullscreen_effect_required_replace_conflict(
    const JceFullscreenEffectPassDesc *a,
    const JceFullscreenEffectPassDesc *b)
{
    if (!a || !b) return false;
    return a->enabled && b->enabled && a->required && b->required &&
           a->blend == JCE_FULLSCREEN_EFFECT_REPLACE &&
           b->blend == JCE_FULLSCREEN_EFFECT_REPLACE &&
           a->insertion == b->insertion && a->order == b->order;
}

uint64_t jce_fullscreen_effect_history_key(
    const JceFullscreenEffectPassDesc *pass,
    const JceFullscreenEffectFrameDesc *frame)
{
    uint64_t hash = JCE_FNV1A64_INIT;
    if (!pass || !frame) return 0;
    hash = jce_fnv1a64_append(hash, pass->shader, strlen(pass->shader));
    hash = jce_fnv1a64_append(hash, &pass->insertion, sizeof(pass->insertion));
    hash = jce_fnv1a64_append(hash, &pass->blend, sizeof(pass->blend));
    hash = jce_fnv1a64_append(hash, &pass->output_format,
                              sizeof(pass->output_format));
    hash = jce_fnv1a64_append(hash, &pass->resolution_scale,
                              sizeof(pass->resolution_scale));
    hash = jce_fnv1a64_append(hash, &pass->texture_count,
                              sizeof(pass->texture_count));
    hash = jce_fnv1a64_append(hash, pass->textures, sizeof(pass->textures));
    hash = jce_fnv1a64_append(hash, pass->params, sizeof(pass->params));
    hash = jce_fnv1a64_append(hash, &frame->width, sizeof(frame->width));
    hash = jce_fnv1a64_append(hash, &frame->height, sizeof(frame->height));
    hash = jce_fnv1a64_append(hash, &frame->history_key,
                              sizeof(frame->history_key));
    hash = jce_fnv1a64_append(hash, &frame->view_proj,
                              sizeof(frame->view_proj));
    hash = jce_fnv1a64_append(hash, &frame->effect_world,
                              sizeof(frame->effect_world));
    return hash;
}

static void copy_mat4(float dst[16], const jce_mat4 *src)
{
    memcpy(dst, &src->raw[0][0], sizeof(float) * 16);
}

bool jce_fullscreen_effect_pack_uniforms(
    const JceFullscreenEffectPassDesc *pass,
    const JceFullscreenEffectFrameDesc *frame,
    uint32_t output_width,
    uint32_t output_height,
    bool history_valid,
    JceFullscreenEffectUniforms *out_uniforms)
{
    if (!pass || !frame || !out_uniforms ||
        pass->struct_size != sizeof(*pass) ||
        frame->struct_size != sizeof(*frame) ||
        frame->width == 0 || frame->height == 0 ||
        output_width == 0 || output_height == 0)
        return false;
    memset(out_uniforms, 0, sizeof(*out_uniforms));
    copy_mat4(out_uniforms->view_proj, &frame->view_proj);
    copy_mat4(out_uniforms->inv_view_proj, &frame->inv_view_proj);
    copy_mat4(out_uniforms->prev_view_proj, &frame->prev_view_proj);
    copy_mat4(out_uniforms->effect_world, &frame->effect_world);
    copy_mat4(out_uniforms->effect_world_inv, &frame->effect_world_inv);
    out_uniforms->camera_position[0] = frame->camera_position.x;
    out_uniforms->camera_position[1] = frame->camera_position.y;
    out_uniforms->camera_position[2] = frame->camera_position.z;
    out_uniforms->camera_position[3] = 1.0f;
    out_uniforms->camera_basis[0][0] = frame->camera_right.x;
    out_uniforms->camera_basis[0][1] = frame->camera_right.y;
    out_uniforms->camera_basis[0][2] = frame->camera_right.z;
    out_uniforms->camera_basis[1][0] = frame->camera_up.x;
    out_uniforms->camera_basis[1][1] = frame->camera_up.y;
    out_uniforms->camera_basis[1][2] = frame->camera_up.z;
    out_uniforms->camera_basis[2][0] = frame->camera_forward.x;
    out_uniforms->camera_basis[2][1] = frame->camera_forward.y;
    out_uniforms->camera_basis[2][2] = frame->camera_forward.z;
    out_uniforms->camera_projection[0] = frame->tan_half_vertical_fov;
    out_uniforms->camera_projection[1] = frame->aspect_ratio;
    out_uniforms->camera_projection[2] = frame->projection_kind;
    out_uniforms->camera_projection[3] = frame->ortho_half_height;
    out_uniforms->viewport[0] = (float)frame->width;
    out_uniforms->viewport[1] = (float)frame->height;
    out_uniforms->viewport[2] = 1.0f / (float)frame->width;
    out_uniforms->viewport[3] = 1.0f / (float)frame->height;
    out_uniforms->output_viewport[0] = (float)output_width;
    out_uniforms->output_viewport[1] = (float)output_height;
    out_uniforms->output_viewport[2] = (float)output_width / (float)frame->width;
    out_uniforms->output_viewport[3] = (float)output_height / (float)frame->height;
    out_uniforms->time_frame[0] = frame->elapsed_sec;
    out_uniforms->time_frame[1] = frame->delta_sec;
    out_uniforms->time_frame[2] = (float)frame->frame_index;
    out_uniforms->time_frame[3] = history_valid ? 1.0f : 0.0f;
    memcpy(out_uniforms->user_params, pass->params,
           sizeof(out_uniforms->user_params));
    return true;
}

static bgfx_texture_format_t to_bgfx_format(uint32_t format)
{
    switch ((JceRenderFormat)format) {
    case JCE_RENDER_FORMAT_RGBA8: return BGFX_TEXTURE_FORMAT_RGBA8;
    case JCE_RENDER_FORMAT_RGBA16F: return BGFX_TEXTURE_FORMAT_RGBA16F;
    case JCE_RENDER_FORMAT_R32F: return BGFX_TEXTURE_FORMAT_R32F;
    case JCE_RENDER_FORMAT_R16F: return BGFX_TEXTURE_FORMAT_R16F;
    case JCE_RENDER_FORMAT_RG16F: return BGFX_TEXTURE_FORMAT_RG16F;
    case JCE_RENDER_FORMAT_RG32F: return BGFX_TEXTURE_FORMAT_RG32F;
    case JCE_RENDER_FORMAT_RGBA32F: return BGFX_TEXTURE_FORMAT_RGBA32F;
    default: return BGFX_TEXTURE_FORMAT_COUNT;
    }
}

static uint32_t sampler_flags(const JceSamplerDesc *sampler)
{
    uint32_t flags = 0;
    if (!sampler) return BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    if (sampler->address_u == JCE_SAMPLER_ADDRESS_CLAMP)
        flags |= BGFX_SAMPLER_U_CLAMP;
    else if (sampler->address_u == JCE_SAMPLER_ADDRESS_MIRROR)
        flags |= BGFX_SAMPLER_U_MIRROR;
    if (sampler->address_v == JCE_SAMPLER_ADDRESS_CLAMP)
        flags |= BGFX_SAMPLER_V_CLAMP;
    else if (sampler->address_v == JCE_SAMPLER_ADDRESS_MIRROR)
        flags |= BGFX_SAMPLER_V_MIRROR;
    if (sampler->filter_min == JCE_SAMPLER_FILTER_NEAREST)
        flags |= BGFX_SAMPLER_MIN_POINT;
    if (sampler->filter_mag == JCE_SAMPLER_FILTER_NEAREST)
        flags |= BGFX_SAMPLER_MAG_POINT;
    if (sampler->filter_mip == JCE_SAMPLER_FILTER_NEAREST)
        flags |= BGFX_SAMPLER_MIP_POINT;
    return flags;
}

static void destroy_targets(JceFullscreenEffectPipeline *pipeline)
{
    uint32_t i;
    for (i = 0; i < 2; i++) {
        if (pipeline->output_target[i].idx != UINT16_MAX)
            bgfx_destroy_frame_buffer(pipeline->output_target[i]);
        if (pipeline->output_texture[i].idx != UINT16_MAX)
            bgfx_destroy_texture(pipeline->output_texture[i]);
        pipeline->output_target[i].idx = UINT16_MAX;
        pipeline->output_texture[i].idx = UINT16_MAX;
    }
    pipeline->target_width = 0;
    pipeline->target_height = 0;
    pipeline->target_format = UINT32_MAX;
    pipeline->write_index = 0;
    pipeline->history_valid = false;
}

static bool ensure_targets(JceFullscreenEffectPipeline *pipeline,
                           uint32_t width, uint32_t height, uint32_t format)
{
    const bgfx_caps_t *caps;
    bgfx_texture_format_t bgfx_format;
    uint32_t i;
    if (pipeline->target_width == width && pipeline->target_height == height &&
        pipeline->target_format == format &&
        pipeline->output_target[0].idx != UINT16_MAX &&
        pipeline->output_target[1].idx != UINT16_MAX)
        return true;

    destroy_targets(pipeline);
    bgfx_format = to_bgfx_format(format);
    caps = bgfx_get_caps();
    if (bgfx_format == BGFX_TEXTURE_FORMAT_COUNT || !caps ||
        !(caps->formats[bgfx_format] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER))
        return false;

    for (i = 0; i < 2; i++) {
        bgfx_attachment_t attachment;
        pipeline->output_texture[i] = bgfx_create_texture_2d(
            (uint16_t)width, (uint16_t)height, false, 1, bgfx_format,
            BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            NULL, 0);
        if (pipeline->output_texture[i].idx == UINT16_MAX) {
            destroy_targets(pipeline);
            return false;
        }
        memset(&attachment, 0, sizeof(attachment));
        bgfx_attachment_init(&attachment, pipeline->output_texture[i],
                             BGFX_ACCESS_WRITE, 0, 1, 0,
                             BGFX_RESOLVE_NONE);
        pipeline->output_target[i] =
            bgfx_create_frame_buffer_from_attachment(1, &attachment, false);
        if (pipeline->output_target[i].idx == UINT16_MAX) {
            destroy_targets(pipeline);
            return false;
        }
    }
    pipeline->target_width = width;
    pipeline->target_height = height;
    pipeline->target_format = format;
    return true;
}

static bool ensure_program(JceFullscreenEffectPipeline *pipeline,
                           const char *shader)
{
    JceShaderHandle loaded;
    if (pipeline->program.idx != UINT16_MAX &&
        strcmp(pipeline->loaded_shader, shader) == 0)
        return true;
    if (pipeline->program.idx != UINT16_MAX)
        bgfx_destroy_program(pipeline->program);
    pipeline->program.idx = UINT16_MAX;
    pipeline->loaded_shader[0] = '\0';
    loaded = shader_load_program_overlay_named(pipeline->shader_dev_dir,
                                                pipeline->project_pak,
                                                "jce_fullscreen", shader);
    if (!jce_shader_valid(loaded)) return false;
    pipeline->program.idx = loaded.idx;
    copy_bounded(pipeline->loaded_shader,
                 sizeof(pipeline->loaded_shader), shader);
    return true;
}

static bgfx_uniform_handle_t create_uniform(const char *name,
                                            bgfx_uniform_type_t type,
                                            uint16_t count)
{
    return bgfx_create_uniform(name, type, count);
}

JceFullscreenEffectPipeline *jce_fullscreen_effect_create(
    jce_allocator_t allocator, const JcePakArchive *project_pak)
{
    JceFullscreenEffectPipeline *pipeline;
    const bgfx_memory_t *memory;
    static const uint32_t white = 0xffffffffu;
    uint32_t i;
    if (!allocator.alloc || !allocator.free) return NULL;
    pipeline = JCE_ANEW(allocator, JceFullscreenEffectPipeline);
    if (!pipeline) return NULL;
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->allocator = allocator;
    pipeline->project_pak = project_pak;
    pipeline->quad_vb.idx = UINT16_MAX;
    pipeline->quad_ib.idx = UINT16_MAX;
    pipeline->program.idx = UINT16_MAX;
    pipeline->fallback_texture.idx = UINT16_MAX;
    pipeline->target_format = UINT32_MAX;
    pipeline->status.struct_size = sizeof(pipeline->status);
    for (i = 0; i < 2; i++) {
        pipeline->output_texture[i].idx = UINT16_MAX;
        pipeline->output_target[i].idx = UINT16_MAX;
    }

    bgfx_vertex_layout_begin(&pipeline->quad_layout,
                             bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&pipeline->quad_layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&pipeline->quad_layout, BGFX_ATTRIB_TEXCOORD0, 2,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&pipeline->quad_layout);
    memory = bgfx_copy(s_quad_vertices, sizeof(s_quad_vertices));
    pipeline->quad_vb = bgfx_create_vertex_buffer(
        memory, &pipeline->quad_layout, BGFX_BUFFER_NONE);
    memory = bgfx_copy(s_quad_indices, sizeof(s_quad_indices));
    pipeline->quad_ib = bgfx_create_index_buffer(memory, BGFX_BUFFER_NONE);
    memory = bgfx_copy(&white, sizeof(white));
    pipeline->fallback_texture = bgfx_create_texture_2d(
        1, 1, false, 1, BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, memory, 0);

    pipeline->s_scene_color = create_uniform(
        "s_jceSceneColor", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    pipeline->s_scene_depth = create_uniform(
        "s_jceSceneDepth", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    pipeline->s_history = create_uniform(
        "s_jceHistory", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    pipeline->s_user[0] = create_uniform(
        "s_jceUser0", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    pipeline->s_user[1] = create_uniform(
        "s_jceUser1", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    pipeline->s_user[2] = create_uniform(
        "s_jceUser2", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    pipeline->s_user[3] = create_uniform(
        "s_jceUser3", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    pipeline->u_view_proj = create_uniform(
        "u_jceViewProj", BGFX_UNIFORM_TYPE_MAT4, 1);
    pipeline->u_inv_view_proj = create_uniform(
        "u_jceInvViewProj", BGFX_UNIFORM_TYPE_MAT4, 1);
    pipeline->u_prev_view_proj = create_uniform(
        "u_jcePrevViewProj", BGFX_UNIFORM_TYPE_MAT4, 1);
    pipeline->u_effect_world = create_uniform(
        "u_jceEffectWorld", BGFX_UNIFORM_TYPE_MAT4, 1);
    pipeline->u_effect_world_inv = create_uniform(
        "u_jceEffectWorldInv", BGFX_UNIFORM_TYPE_MAT4, 1);
    pipeline->u_camera_position = create_uniform(
        "u_jceCameraPosition", BGFX_UNIFORM_TYPE_VEC4, 1);
    pipeline->u_camera_basis = create_uniform(
        "u_jceCameraBasis", BGFX_UNIFORM_TYPE_VEC4, 3);
    pipeline->u_camera_projection = create_uniform(
        "u_jceCameraProjection", BGFX_UNIFORM_TYPE_VEC4, 1);
    pipeline->u_viewport = create_uniform(
        "u_jceViewport", BGFX_UNIFORM_TYPE_VEC4, 1);
    pipeline->u_output_viewport = create_uniform(
        "u_jceOutputViewport", BGFX_UNIFORM_TYPE_VEC4, 1);
    pipeline->u_time_frame = create_uniform(
        "u_jceTimeFrame", BGFX_UNIFORM_TYPE_VEC4, 1);
    pipeline->u_params = create_uniform(
        "u_jceParams", BGFX_UNIFORM_TYPE_VEC4,
        JCE_FULLSCREEN_EFFECT_MAX_PARAMS);
    return pipeline;
}

static void destroy_uniform(bgfx_uniform_handle_t handle)
{
    if (handle.idx != UINT16_MAX) bgfx_destroy_uniform(handle);
}

void jce_fullscreen_effect_destroy(JceFullscreenEffectPipeline *pipeline)
{
    uint32_t i;
    if (!pipeline) return;
    destroy_targets(pipeline);
    if (pipeline->program.idx != UINT16_MAX)
        bgfx_destroy_program(pipeline->program);
    if (pipeline->quad_vb.idx != UINT16_MAX)
        bgfx_destroy_vertex_buffer(pipeline->quad_vb);
    if (pipeline->quad_ib.idx != UINT16_MAX)
        bgfx_destroy_index_buffer(pipeline->quad_ib);
    if (pipeline->fallback_texture.idx != UINT16_MAX)
        bgfx_destroy_texture(pipeline->fallback_texture);
    destroy_uniform(pipeline->s_scene_color);
    destroy_uniform(pipeline->s_scene_depth);
    destroy_uniform(pipeline->s_history);
    for (i = 0; i < JCE_FULLSCREEN_EFFECT_MAX_TEXTURES; i++)
        destroy_uniform(pipeline->s_user[i]);
    destroy_uniform(pipeline->u_view_proj);
    destroy_uniform(pipeline->u_inv_view_proj);
    destroy_uniform(pipeline->u_prev_view_proj);
    destroy_uniform(pipeline->u_effect_world);
    destroy_uniform(pipeline->u_effect_world_inv);
    destroy_uniform(pipeline->u_camera_position);
    destroy_uniform(pipeline->u_camera_basis);
    destroy_uniform(pipeline->u_camera_projection);
    destroy_uniform(pipeline->u_viewport);
    destroy_uniform(pipeline->u_output_viewport);
    destroy_uniform(pipeline->u_time_frame);
    destroy_uniform(pipeline->u_params);
    JCE_AFREE(pipeline->allocator, pipeline);
}

void jce_fullscreen_effect_set_shader_dev_dir(
    JceFullscreenEffectPipeline *pipeline, const char *dev_dir)
{
    char next[sizeof(pipeline->shader_dev_dir)];
    if (!pipeline) return;
    copy_bounded(next, sizeof(next), dev_dir);
    if (strcmp(next, pipeline->shader_dev_dir) == 0) return;
    copy_bounded(pipeline->shader_dev_dir,
                 sizeof(pipeline->shader_dev_dir), next);
    if (pipeline->program.idx != UINT16_MAX)
        bgfx_destroy_program(pipeline->program);
    pipeline->program.idx = UINT16_MAX;
    pipeline->loaded_shader[0] = '\0';
    jce_fullscreen_effect_reset_history(pipeline);
}

static JceTextureHandle fail_apply(JceFullscreenEffectPipeline *pipeline,
                                   const JceFullscreenEffectPassDesc *pass,
                                   const JceFullscreenEffectFrameDesc *frame,
                                   JceFullscreenEffectError error)
{
    pipeline->status.result = pass && pass->required
        ? JCE_FULLSCREEN_EFFECT_RESULT_REQUIRED_FAILED
        : JCE_FULLSCREEN_EFFECT_RESULT_OPTIONAL_FAILED;
    pipeline->status.error = error;
    pipeline->history_valid = false;
    return frame ? frame->scene_color : JCE_INVALID_TEXTURE;
}

JceTextureHandle jce_fullscreen_effect_apply(
    JceFullscreenEffectPipeline *pipeline,
    const JceFullscreenEffectPassDesc *pass,
    const JceFullscreenEffectFrameDesc *frame)
{
    JceFullscreenEffectPassDesc clean;
    JceFullscreenEffectUniforms uniforms;
    bgfx_texture_handle_t scene_color, scene_depth, history;
    bgfx_frame_buffer_handle_t target;
    uint32_t output_width, output_height, i;
    uint64_t key, state;
    bool history_valid;
    uint8_t write_index, history_index;
    jce_mat4 identity;

    if (!pipeline || !pass || !frame) return JCE_INVALID_TEXTURE;
    memset(&pipeline->status, 0, sizeof(pipeline->status));
    pipeline->status.struct_size = sizeof(pipeline->status);
    copy_bounded(pipeline->status.logical_shader,
                 sizeof(pipeline->status.logical_shader), pass->shader);
    copy_bounded(pipeline->status.backend, sizeof(pipeline->status.backend),
                 jce_shaders_backend_suffix());
    if (!pass->enabled) {
        pipeline->status.result = JCE_FULLSCREEN_EFFECT_RESULT_BYPASSED;
        return frame->scene_color;
    }
    clean = *pass;
    if (!jce_fullscreen_effect_sanitize(&clean) ||
        frame->struct_size != sizeof(*frame) || frame->width == 0 ||
        frame->height == 0)
        return fail_apply(pipeline, pass, frame,
                          JCE_FULLSCREEN_EFFECT_ERROR_INVALID_DESC);
    if (clean.insertion != JCE_FULLSCREEN_EFFECT_HDR_BEFORE_POSTFX)
        return fail_apply(pipeline, &clean, frame,
                          JCE_FULLSCREEN_EFFECT_ERROR_UNSUPPORTED_INSERTION);
    if (clean.use_scene_color &&
        !jce_gfx_texture_valid(frame->scene_color))
        return fail_apply(pipeline, &clean, frame,
                          JCE_FULLSCREEN_EFFECT_ERROR_MISSING_TEXTURE);
    if (clean.use_scene_depth &&
        !jce_gfx_texture_valid(frame->scene_depth))
        return fail_apply(pipeline, &clean, frame,
                          JCE_FULLSCREEN_EFFECT_ERROR_MISSING_TEXTURE);
    for (i = 0; i < clean.texture_count; i++) {
        if (!jce_gfx_texture_valid(clean.textures[i]))
            return fail_apply(pipeline, &clean, frame,
                              JCE_FULLSCREEN_EFFECT_ERROR_MISSING_TEXTURE);
    }
    if (!ensure_program(pipeline, clean.shader))
        return fail_apply(pipeline, &clean, frame,
                          JCE_FULLSCREEN_EFFECT_ERROR_MISSING_SHADER);
    jce_fullscreen_effect_target_extent(clean.resolution_scale,
                                        frame->width, frame->height,
                                        &output_width, &output_height);
    if (!ensure_targets(pipeline, output_width, output_height,
                        clean.output_format))
        return fail_apply(pipeline, &clean, frame,
                          color_format_valid(clean.output_format)
                              ? JCE_FULLSCREEN_EFFECT_ERROR_TARGET_ALLOCATION
                              : JCE_FULLSCREEN_EFFECT_ERROR_UNSUPPORTED_FORMAT);

    key = jce_fullscreen_effect_history_key(&clean, frame);
    history_valid = clean.use_history && pipeline->history_valid &&
                    pipeline->history_key == key;
    if (!jce_fullscreen_effect_pack_uniforms(&clean, frame,
            output_width, output_height, history_valid, &uniforms))
        return fail_apply(pipeline, &clean, frame,
                          JCE_FULLSCREEN_EFFECT_ERROR_INVALID_DESC);

    JCE_PROFILE_ZONE_N("FullscreenEffect::Apply");
    write_index = pipeline->write_index;
    history_index = (uint8_t)(1u - write_index);
    target = pipeline->output_target[write_index];
    scene_color.idx = jce_gfx_texture_valid(frame->scene_color)
        ? frame->scene_color.idx : pipeline->fallback_texture.idx;
    scene_depth.idx = jce_gfx_texture_valid(frame->scene_depth)
        ? frame->scene_depth.idx : pipeline->fallback_texture.idx;
    history = history_valid ? pipeline->output_texture[history_index]
                            : pipeline->fallback_texture;

    bgfx_set_view_name(frame->view_id, "JCE Fullscreen Effect", INT32_MAX);
    bgfx_set_view_mode(frame->view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_set_view_rect(frame->view_id, 0, 0,
                       (uint16_t)output_width, (uint16_t)output_height);
    bgfx_set_view_frame_buffer(frame->view_id, target);
    bgfx_set_view_clear(frame->view_id, BGFX_CLEAR_COLOR,
                        0x00000000, 1.0f, 0);
    identity = jce_m4_identity();
    bgfx_set_view_transform(frame->view_id, JCE_M4_PTR(identity),
                            JCE_M4_PTR(identity));
    bgfx_set_transform(JCE_M4_PTR(identity), 1);

    bgfx_set_uniform(pipeline->u_view_proj, uniforms.view_proj, 1);
    bgfx_set_uniform(pipeline->u_inv_view_proj, uniforms.inv_view_proj, 1);
    bgfx_set_uniform(pipeline->u_prev_view_proj, uniforms.prev_view_proj, 1);
    bgfx_set_uniform(pipeline->u_effect_world, uniforms.effect_world, 1);
    bgfx_set_uniform(pipeline->u_effect_world_inv, uniforms.effect_world_inv, 1);
    bgfx_set_uniform(pipeline->u_camera_position,
                     uniforms.camera_position, 1);
    bgfx_set_uniform(pipeline->u_camera_basis, uniforms.camera_basis, 3);
    bgfx_set_uniform(pipeline->u_camera_projection,
                     uniforms.camera_projection, 1);
    bgfx_set_uniform(pipeline->u_viewport, uniforms.viewport, 1);
    bgfx_set_uniform(pipeline->u_output_viewport,
                     uniforms.output_viewport, 1);
    bgfx_set_uniform(pipeline->u_time_frame, uniforms.time_frame, 1);
    bgfx_set_uniform(pipeline->u_params, uniforms.user_params,
                     JCE_FULLSCREEN_EFFECT_MAX_PARAMS);

    bgfx_set_texture(0, pipeline->s_scene_color, scene_color,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    bgfx_set_texture(1, pipeline->s_scene_depth, scene_depth,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
                     BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                     BGFX_SAMPLER_MIP_POINT);
    bgfx_set_texture(2, pipeline->s_history, history,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    for (i = 0; i < JCE_FULLSCREEN_EFFECT_MAX_TEXTURES; i++) {
        bgfx_texture_handle_t texture = pipeline->fallback_texture;
        if (i < clean.texture_count &&
            jce_gfx_texture_valid(clean.textures[i]))
            texture.idx = clean.textures[i].idx;
        bgfx_set_texture(s_user_stage[i], pipeline->s_user[i], texture,
                         sampler_flags(&clean.samplers[i]));
    }

    state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
    if (clean.blend == JCE_FULLSCREEN_EFFECT_ADD)
        state |= BGFX_STATE_BLEND_ADD;
    else if (clean.blend == JCE_FULLSCREEN_EFFECT_ALPHA)
        state |= BGFX_STATE_BLEND_ALPHA;
    bgfx_set_vertex_buffer(0, pipeline->quad_vb, 0, 4);
    bgfx_set_index_buffer(pipeline->quad_ib, 0, 6);
    bgfx_set_state(state, 0);
    bgfx_submit(frame->view_id, pipeline->program, 0, BGFX_DISCARD_ALL);

    pipeline->history_key = key;
    pipeline->history_valid = clean.use_history;
    pipeline->write_index = history_index;
    pipeline->status.result = JCE_FULLSCREEN_EFFECT_RESULT_APPLIED;
    pipeline->status.error = JCE_FULLSCREEN_EFFECT_ERROR_NONE;
    pipeline->status.output_width = output_width;
    pipeline->status.output_height = output_height;
    pipeline->status.history_valid = history_valid;
    JCE_PROFILE_ZONE_END;
    return (JceTextureHandle){ pipeline->output_texture[write_index].idx };
}

void jce_fullscreen_effect_reset_history(
    JceFullscreenEffectPipeline *pipeline)
{
    if (!pipeline) return;
    pipeline->history_valid = false;
    pipeline->history_key = 0;
}

void jce_fullscreen_effect_get_status(
    const JceFullscreenEffectPipeline *pipeline,
    JceFullscreenEffectStatus *out_status)
{
    if (!out_status) return;
    memset(out_status, 0, sizeof(*out_status));
    out_status->struct_size = sizeof(*out_status);
    if (pipeline) *out_status = pipeline->status;
}
