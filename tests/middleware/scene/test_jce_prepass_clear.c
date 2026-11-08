/* Actual MRT clear calls and target owner, with counted bgfx ports only.
 * Shader output indices are derived from both actual velocity fragment sources
 * at configure time. No window, device, shader execution or pixel claim. */
#include "unity.h"
#include "jce_sr_prepass_targets.h"
#include <jce/os/core/jce_trace.h>
#include <bgfx/c99/bgfx.h>
#include <string.h>

typedef struct {
    uint16_t ssao_w, ssao_h;
    bgfx_frame_buffer_handle_t ssao_depth_fbo;
    bgfx_texture_handle_t ssao_normal_tex, ssao_albedo_tex;
} ClearOwner;

static SrPrepassTargets targets;
static uint16_t next_texture, attachments[4];
static uint8_t attachment_count, clear_indices[8];
static unsigned rect_calls, frame_buffer_calls, single_calls, mrt_calls;
static unsigned palette_calls, textures_created, textures_destroyed, fb_created, fb_destroyed;
static unsigned target_frame_buffer_attempts, depth_only_refusals;
static bool refuse_depth_only;
static float palette[3][4], clear_depth;
static bool palette_written[3];
static uint16_t clear_flags;
static uint8_t clear_stencil;
static uint32_t single_rgba;
static const uint16_t view = 17;

static uint16_t create_texture(uint16_t w, uint16_t h, uint16_t format, void *user)
{
    (void)format; (void)user;
    TEST_ASSERT_EQUAL_UINT16(2560, w); TEST_ASSERT_EQUAL_UINT16(1440, h);
    ++textures_created;
    return next_texture++;
}
static uint16_t create_frame_buffer(const uint16_t *handles, uint8_t count, void *user)
{
    (void)user; TEST_ASSERT_TRUE(count == 1 || count == 3 || count == 4);
    ++target_frame_buffer_attempts;
    if (count == 1 && refuse_depth_only) {
        ++depth_only_refusals; return UINT16_MAX;
    }
    ++fb_created;
    memcpy(attachments, handles, count * sizeof *handles);
    attachment_count = count;
    return 0; /* Handle zero is a real owned handle. */
}
static void destroy_texture(uint16_t handle, void *user)
{ (void)handle; (void)user; ++textures_destroyed; }
static void destroy_frame_buffer(uint16_t handle, void *user)
{ (void)user; TEST_ASSERT_EQUAL_UINT16(0, handle); ++fb_destroyed; }
static const SrPrepassTargetProvider provider = {
    create_texture, create_frame_buffer, destroy_texture, destroy_frame_buffer, NULL
};

static void rect_port(bgfx_view_id_t id, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    TEST_ASSERT_EQUAL_UINT16(view, id); TEST_ASSERT_EQUAL_UINT16(0, x);
    TEST_ASSERT_EQUAL_UINT16(0, y); TEST_ASSERT_EQUAL_UINT16(2560, w);
    TEST_ASSERT_EQUAL_UINT16(1440, h); ++rect_calls;
}
static void frame_buffer_port(bgfx_view_id_t id, bgfx_frame_buffer_handle_t handle)
{ TEST_ASSERT_EQUAL_UINT16(view, id); TEST_ASSERT_EQUAL_UINT16(0, handle.idx); ++frame_buffer_calls; }
static void palette_float_port(uint8_t index, const float *rgba)
{
    TEST_ASSERT_LESS_THAN_UINT8(3, index); TEST_ASSERT_NOT_NULL(rgba);
    memcpy(palette[index], rgba, sizeof palette[index]);
    palette_written[index] = true; ++palette_calls;
}
static void palette_rgba8_port(uint8_t index, uint32_t rgba)
{
    float values[4];
    for (unsigned i = 0; i < 4; ++i)
        values[i] = (float)((rgba >> ((3u-i)*8u)) & 255u) / 255.0f;
    palette_float_port(index, values);
}
static void single_port(bgfx_view_id_t id, uint16_t flags, uint32_t rgba,
                         float depth, uint8_t stencil)
{
    TEST_ASSERT_EQUAL_UINT16(view, id); ++single_calls;
    clear_flags = flags; single_rgba = rgba; clear_depth = depth; clear_stencil = stencil;
}
static void mrt_port(bgfx_view_id_t id, uint16_t flags, float depth, uint8_t stencil,
    uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3,
    uint8_t c4, uint8_t c5, uint8_t c6, uint8_t c7)
{
    TEST_ASSERT_EQUAL_UINT16(view, id); ++mrt_calls;
    const uint8_t indices[8] = {c0,c1,c2,c3,c4,c5,c6,c7};
    memcpy(clear_indices, indices, sizeof indices);
    clear_flags = flags; clear_depth = depth; clear_stencil = stencil;
}

/* Actual declarations precede typed function-name ports. */
#define bgfx_set_view_rect rect_port
#define bgfx_set_view_frame_buffer frame_buffer_port
#define bgfx_set_palette_color_rgba8 palette_rgba8_port
#define bgfx_set_palette_color palette_float_port
#define bgfx_set_view_clear single_port
#define bgfx_set_view_clear_mrt mrt_port
static void clear_actual(const SrPrepassTarget *target, bool write_velocity)
{
    ClearOwner state = {(uint16_t)target->request.width, (uint16_t)target->request.height,
                        {target->frame_buffer}, {target->normal}, {target->albedo}};
    ClearOwner *sr = &state;
    const uint16_t v = view;
#include JCE_TEST_PREPASS_CLEAR_OWNER
}
#undef bgfx_set_view_rect
#undef bgfx_set_view_frame_buffer
#undef bgfx_set_palette_color_rgba8
#undef bgfx_set_palette_color
#undef bgfx_set_view_clear
#undef bgfx_set_view_clear_mrt

/* The complete production adapter owns validation-before-creation. These
 * typed ports observe its actual attachments; no layout policy is duplicated. */
static unsigned adapter_attachment_calls, adapter_query_calls, adapter_create_calls;
static bool adapter_supported;
static uint8_t adapter_expected_count;
static const uint16_t adapter_inputs[4] = {0, 13, 21, 34};
static void attachment_init_port(bgfx_attachment_t *attachment,
    bgfx_texture_handle_t texture, bgfx_access_t access, uint16_t layer,
    uint16_t num_layers, uint16_t mip, uint8_t resolve)
{
    TEST_ASSERT_LESS_THAN_UINT(4, adapter_attachment_calls);
    TEST_ASSERT_EQUAL_UINT16(adapter_inputs[adapter_attachment_calls], texture.idx);
    TEST_ASSERT_EQUAL_INT(BGFX_ACCESS_WRITE, access);
    TEST_ASSERT_EQUAL_UINT16(0, layer); TEST_ASSERT_EQUAL_UINT16(1, num_layers);
    TEST_ASSERT_EQUAL_UINT16(0, mip); TEST_ASSERT_EQUAL_UINT8(BGFX_RESOLVE_NONE, resolve);
    attachment->handle = texture; attachment->access = access;
    attachment->layer = layer; attachment->numLayers = num_layers;
    attachment->mip = mip; attachment->resolve = resolve;
    ++adapter_attachment_calls;
}
static void assert_adapter_attachments(uint8_t count, const bgfx_attachment_t *values)
{
    TEST_ASSERT_NOT_NULL(values); TEST_ASSERT_EQUAL_UINT8(adapter_expected_count, count);
    TEST_ASSERT_EQUAL_UINT(count, adapter_attachment_calls);
    for (uint8_t i = 0; i < count; ++i) {
        TEST_ASSERT_EQUAL_UINT16(adapter_inputs[i], values[i].handle.idx);
        TEST_ASSERT_EQUAL_INT(BGFX_ACCESS_WRITE, values[i].access);
        TEST_ASSERT_EQUAL_UINT16(0, values[i].layer);
        TEST_ASSERT_EQUAL_UINT16(1, values[i].numLayers);
        TEST_ASSERT_EQUAL_UINT16(0, values[i].mip);
        TEST_ASSERT_EQUAL_UINT8(BGFX_RESOLVE_NONE, values[i].resolve);
    }
}
static bool frame_buffer_valid_port(uint8_t count, const bgfx_attachment_t *values)
{
    TEST_ASSERT_EQUAL_UINT8(1, count); assert_adapter_attachments(count, values);
    TEST_ASSERT_EQUAL_UINT(0, adapter_create_calls); ++adapter_query_calls;
    return adapter_supported;
}
static bgfx_frame_buffer_handle_t frame_buffer_create_port(uint8_t count,
    const bgfx_attachment_t *values, bool destroy_textures)
{
    assert_adapter_attachments(count, values); TEST_ASSERT_FALSE(destroy_textures);
    ++adapter_create_calls;
    const bgfx_frame_buffer_handle_t result = {37}; return result;
}
#define bgfx_attachment_init attachment_init_port
#define bgfx_is_frame_buffer_valid frame_buffer_valid_port
#define bgfx_create_frame_buffer_from_attachment frame_buffer_create_port
#include JCE_TEST_PREPASS_ADAPTER_OWNER
#undef bgfx_attachment_init
#undef bgfx_is_frame_buffer_valid
#undef bgfx_create_frame_buffer_from_attachment

void setUp(void)
{
    jce_trace_set_enabled(false); sr_prepass_targets_init(&targets);
    next_texture = 0; attachment_count = 0;
    adapter_attachment_calls = adapter_query_calls = adapter_create_calls = 0;
    adapter_expected_count = 0; adapter_supported = true;
    rect_calls = frame_buffer_calls = single_calls = mrt_calls = palette_calls = 0;
    textures_created = textures_destroyed = fb_created = fb_destroyed = 0;
    target_frame_buffer_attempts = depth_only_refusals = 0; refuse_depth_only = false;
    clear_flags = 0; clear_depth = 0.0f; clear_stencil = 0; single_rgba = 0;
    memset(palette, 0, sizeof palette); memset(palette_written, 0, sizeof palette_written);
    memset(clear_indices, UINT8_MAX, sizeof clear_indices);
}
void tearDown(void)
{
    sr_prepass_targets_release(&targets, &provider, &targets, SR_PREPASS_TARGET_DESTROY);
    TEST_ASSERT_EQUAL_UINT(textures_created, textures_destroyed);
    TEST_ASSERT_EQUAL_UINT(fb_created, fb_destroyed);
}
static const SrPrepassTarget *acquire_mode(bool velocity, bool prefer_depth_only)
{
    const SrPrepassTargetRequest request = {view, 2560, 1440,
        BGFX_TEXTURE_FORMAT_D24S8, BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_FORMAT_RGBA16F, velocity, prefer_depth_only};
    const SrPrepassTarget *target = sr_prepass_targets_acquire(&targets, &request, &provider, &targets);
    TEST_ASSERT_NOT_NULL(target); return target;
}
static const SrPrepassTarget *acquire(bool velocity)
{ return acquire_mode(velocity, false); }
static const float *cleared_attachment(uint8_t index)
{
    TEST_ASSERT_LESS_THAN_UINT8(attachment_count-1u, index); /* Exclude depth. */
    const uint8_t selected = clear_indices[index];
    TEST_ASSERT_LESS_THAN_UINT8_MESSAGE(3, selected,
        "Every live color attachment needs an explicit initialized clear palette");
    TEST_ASSERT_TRUE(palette_written[selected]);
    return palette[selected];
}
static void assert_frame_clear(uint16_t expected_flags)
{
    TEST_ASSERT_EQUAL_UINT(1, rect_calls); TEST_ASSERT_EQUAL_UINT(1, frame_buffer_calls);
    TEST_ASSERT_EQUAL_UINT16(expected_flags, clear_flags);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, clear_depth); TEST_ASSERT_EQUAL_UINT8(0, clear_stencil);
}
static void test_velocity_clear_matches_target_and_both_shader_outputs(void)
{
    const SrPrepassTarget *target = acquire(true); clear_actual(target, true);
    assert_frame_clear(BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH|BGFX_CLEAR_STENCIL); TEST_ASSERT_EQUAL_UINT(1, mrt_calls); TEST_ASSERT_EQUAL_UINT(0, single_calls);
    TEST_ASSERT_EQUAL_UINT8(4, attachment_count);
    TEST_ASSERT_LESS_THAN_UINT8(3, JCE_TEST_STATIC_ALBEDO_SLOT);
    TEST_ASSERT_LESS_THAN_UINT8(3, JCE_TEST_SKINNED_ALBEDO_SLOT);
    TEST_ASSERT_LESS_THAN_UINT8(3, JCE_TEST_STATIC_VELOCITY_SLOT);
    TEST_ASSERT_LESS_THAN_UINT8(3, JCE_TEST_SKINNED_VELOCITY_SLOT);
    TEST_ASSERT_EQUAL_UINT16(target->normal, attachments[0]);
    TEST_ASSERT_EQUAL_UINT16(target->albedo, attachments[JCE_TEST_STATIC_ALBEDO_SLOT]);
    TEST_ASSERT_EQUAL_UINT16(target->albedo, attachments[JCE_TEST_SKINNED_ALBEDO_SLOT]);
    TEST_ASSERT_EQUAL_UINT16(target->velocity, attachments[JCE_TEST_STATIC_VELOCITY_SLOT]);
    TEST_ASSERT_EQUAL_UINT16(target->velocity, attachments[JCE_TEST_SKINNED_VELOCITY_SLOT]);
    TEST_ASSERT_EQUAL_UINT16(target->depth, attachments[3]);
    TEST_ASSERT_EQUAL_UINT(3, palette_calls);
    const float *normal = cleared_attachment(0);
    TEST_ASSERT_EQUAL_FLOAT(128.0f/255.0f, normal[0]);
    TEST_ASSERT_EQUAL_FLOAT(128.0f/255.0f, normal[1]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, normal[2]); TEST_ASSERT_EQUAL_FLOAT(1.0f, normal[3]);
    const float *albedo = cleared_attachment(JCE_TEST_STATIC_ALBEDO_SLOT);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, albedo[0]); TEST_ASSERT_EQUAL_FLOAT(0.0f, albedo[1]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, albedo[2]); TEST_ASSERT_EQUAL_FLOAT(1.0f, albedo[3]);
    const float *velocity = cleared_attachment(JCE_TEST_STATIC_VELOCITY_SLOT);
    TEST_ASSERT_TRUE(velocity[0] == 0.5f && velocity[1] == 0.5f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, velocity[0]*2.0f-1.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, velocity[1]*2.0f-1.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, velocity[2]); TEST_ASSERT_EQUAL_FLOAT(1.0f, velocity[3]);
    for (unsigned i = 3; i < 8; ++i) TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, clear_indices[i]);
}
static void test_without_velocity_retains_existing_single_color_clear(void)
{
    const SrPrepassTarget *target = acquire(false); clear_actual(target, false);
    assert_frame_clear(BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH|BGFX_CLEAR_STENCIL); TEST_ASSERT_EQUAL_UINT(0, mrt_calls); TEST_ASSERT_EQUAL_UINT(1, single_calls);
    TEST_ASSERT_EQUAL_UINT(0, palette_calls); TEST_ASSERT_EQUAL_UINT8(3, attachment_count);
    TEST_ASSERT_EQUAL_UINT16(target->normal, attachments[0]);
    TEST_ASSERT_EQUAL_UINT16(target->albedo, attachments[1]);
    TEST_ASSERT_EQUAL_UINT16(target->depth, attachments[2]);
    TEST_ASSERT_EQUAL_UINT32(0x8080ffffu, single_rgba);
}
static void test_actual_depth_only_target_clears_depth_stencil_without_color(void)
{
    const SrPrepassTarget *target = acquire_mode(false, true);
    TEST_ASSERT_TRUE(target->request.prefer_depth_only);
    TEST_ASSERT_EQUAL_UINT(1, textures_created); TEST_ASSERT_EQUAL_UINT8(1, attachment_count);
    TEST_ASSERT_EQUAL_UINT16(0, target->depth);
    TEST_ASSERT_EQUAL_UINT16(target->depth, attachments[0]);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->normal);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->albedo);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->velocity);
    clear_actual(target, false);
    assert_frame_clear(BGFX_CLEAR_DEPTH|BGFX_CLEAR_STENCIL);
    TEST_ASSERT_EQUAL_UINT(1, single_calls); TEST_ASSERT_EQUAL_UINT(0, mrt_calls);
    TEST_ASSERT_EQUAL_UINT(0, palette_calls);
}
static void test_refused_depth_only_fbo_uses_actual_mrt_fallback_color_clear(void)
{
    refuse_depth_only = true;
    const SrPrepassTarget *target = acquire_mode(false, true);
    TEST_ASSERT_TRUE(target->request.prefer_depth_only);
    TEST_ASSERT_TRUE(target->normal != UINT16_MAX && target->albedo != UINT16_MAX);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->velocity);
    TEST_ASSERT_EQUAL_UINT(3, textures_created); TEST_ASSERT_EQUAL_UINT8(3, attachment_count);
    TEST_ASSERT_EQUAL_UINT(2, target_frame_buffer_attempts);
    TEST_ASSERT_EQUAL_UINT(1, depth_only_refusals); TEST_ASSERT_EQUAL_UINT(1, fb_created);
    TEST_ASSERT_EQUAL_UINT16(target->normal, attachments[0]);
    TEST_ASSERT_EQUAL_UINT16(target->albedo, attachments[1]);
    TEST_ASSERT_EQUAL_UINT16(target->depth, attachments[2]);
    const uint64_t generation = target->generation;
    target = acquire_mode(false, true);
    TEST_ASSERT_EQUAL_UINT64(generation, target->generation);
    TEST_ASSERT_EQUAL_UINT(2, target_frame_buffer_attempts); TEST_ASSERT_EQUAL_UINT(3, textures_created);
    clear_actual(target, false);
    assert_frame_clear(BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH|BGFX_CLEAR_STENCIL);
    TEST_ASSERT_EQUAL_UINT(1, single_calls); TEST_ASSERT_EQUAL_UINT(0, mrt_calls);
    TEST_ASSERT_EQUAL_UINT(0, palette_calls); TEST_ASSERT_EQUAL_UINT32(0x8080ffffu, single_rgba);
}
static void test_velocity_keeps_mrt_color_clear_with_depth_only_preference(void)
{
    const SrPrepassTarget *target = acquire_mode(true, true);
    TEST_ASSERT_TRUE(target->request.prefer_depth_only);
    TEST_ASSERT_EQUAL_UINT8(4, attachment_count); TEST_ASSERT_EQUAL_UINT(4, textures_created);
    clear_actual(target, true);
    assert_frame_clear(BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH|BGFX_CLEAR_STENCIL);
    TEST_ASSERT_EQUAL_UINT(1, mrt_calls); TEST_ASSERT_EQUAL_UINT(0, single_calls);
    TEST_ASSERT_EQUAL_UINT(3, palette_calls);
    const float *velocity = cleared_attachment(JCE_TEST_STATIC_VELOCITY_SLOT);
    TEST_ASSERT_TRUE(velocity[0] == 0.5f && velocity[1] == 0.5f);
}
static void test_actual_adapter_refuses_unsupported_depth_only_before_create(void)
{
    adapter_expected_count = 1; adapter_supported = false;
    const uint16_t result = frame_buffer_create(adapter_inputs, 1, NULL);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, result);
    TEST_ASSERT_EQUAL_UINT(1, adapter_attachment_calls);
    TEST_ASSERT_EQUAL_UINT(1, adapter_query_calls); TEST_ASSERT_EQUAL_UINT(0, adapter_create_calls);
}
static void test_actual_adapter_admits_supported_depth_only_once(void)
{
    adapter_expected_count = 1;
    TEST_ASSERT_EQUAL_UINT16(37, frame_buffer_create(adapter_inputs, 1, NULL));
    TEST_ASSERT_EQUAL_UINT(1, adapter_attachment_calls);
    TEST_ASSERT_EQUAL_UINT(1, adapter_query_calls); TEST_ASSERT_EQUAL_UINT(1, adapter_create_calls);
}
static void test_actual_adapter_preserves_legacy_three_and_four_attachment_create(void)
{
    adapter_supported = false; /* Legacy creation never makes the new layout query. */
    for (uint8_t count = 3; count <= 4; ++count) {
        adapter_attachment_calls = adapter_query_calls = adapter_create_calls = 0;
        adapter_expected_count = count;
        TEST_ASSERT_EQUAL_UINT16(37, frame_buffer_create(adapter_inputs, count, NULL));
        TEST_ASSERT_EQUAL_UINT(count, adapter_attachment_calls);
        TEST_ASSERT_EQUAL_UINT(0, adapter_query_calls); TEST_ASSERT_EQUAL_UINT(1, adapter_create_calls);
    }
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_velocity_clear_matches_target_and_both_shader_outputs);
    RUN_TEST(test_without_velocity_retains_existing_single_color_clear);
    RUN_TEST(test_actual_depth_only_target_clears_depth_stencil_without_color);
    RUN_TEST(test_refused_depth_only_fbo_uses_actual_mrt_fallback_color_clear);
    RUN_TEST(test_velocity_keeps_mrt_color_clear_with_depth_only_preference);
    RUN_TEST(test_actual_adapter_refuses_unsupported_depth_only_before_create);
    RUN_TEST(test_actual_adapter_admits_supported_depth_only_once);
    RUN_TEST(test_actual_adapter_preserves_legacy_three_and_four_attachment_create);
    return UNITY_END();
}
