/* Reuse the actual-TU fixture's pinned bgfx reference provider, not a second
 * ownership model. Only ordinary constructor resources use Noop. The private
 * production allocation routines are called directly; no shader/GPU draws. */
#include <bgfx/c99/bgfx.h>
#include <jce/renderer/jce_renderer_caps.h>

static bool override_profile;
static JceGraphicsApiTier profile_tier;
static bgfx_renderer_type_t profile_backend;
static unsigned postfx_submissions;

static JceRendererApiInfo test_postfx_api_info(void)
{
    JceRendererApiInfo api = jce_renderer_get_api_info();
    if (override_profile) api.build_tier = profile_tier;
    return api;
}

static bgfx_renderer_type_t test_postfx_backend(void)
{
    return override_profile ? profile_backend : bgfx_get_renderer_type();
}

static void test_postfx_submit(bgfx_view_id_t view, bgfx_program_handle_t program,
                               uint32_t depth, uint8_t flags)
{
    (void)view; (void)program; (void)depth; (void)flags;
    ++postfx_submissions;
}

#define jce_renderer_get_api_info test_postfx_api_info
#define bgfx_get_renderer_type test_postfx_backend
#define bgfx_submit test_postfx_submit
#define JCE_POSTFX_OWNERSHIP_SUPPORT_ONLY
#include "test_jce_postfx_tsr_ownership.c"
#undef bgfx_submit
#undef bgfx_get_renderer_type
#undef jce_renderer_get_api_info

static void assert_no_fake_owners(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, bad_releases);
    for (unsigned i = 0; i < texture_count; ++i) {
        TEST_ASSERT_EQUAL_INT(0, textures[i].refs);
        TEST_ASSERT_EQUAL_UINT32(1, textures[i].destroy_requests);
    }
    for (unsigned i = 0; i < framebuffer_count; ++i)
        TEST_ASSERT_TRUE(framebuffers[i].destroyed);
}
static void assert_taa_targets_empty(void)
{
    TEST_ASSERT_FALSE(pipeline->taa_fbos_valid);
    TEST_ASSERT_FALSE(pipeline->history_valid);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX,pipeline->taa_history_tex.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX,pipeline->taa_history_fb.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX,pipeline->taa_motion_tex.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX,pipeline->taa_motion_fb.idx);
}
static void enable_motion_only(void)
{
    create_pipeline();
    jce_postfx_set_taa(pipeline,false,0.9f,1.0f,1.0f);
    jce_postfx_set_motion_blur(pipeline,true,1.0f);
    TEST_ASSERT_TRUE(jce_postfx_get_motion_blur(pipeline));
    TEST_ASSERT_FALSE(pipeline->taa_enabled);
}
static void test_motion_only_resize_retires_then_recreates(void)
{
    enable_motion_only();
    TEST_ASSERT_TRUE(ensure_taa_motion_fbo(pipeline));
    TEST_ASSERT_FALSE(pipeline->taa_fbos_valid);
    const uint16_t before = pipeline->taa_motion_fb.idx;
    const unsigned calls = fbo_attempts;
    TEST_ASSERT_TRUE(ensure_taa_motion_fbo(pipeline));
    TEST_ASSERT_EQUAL_UINT32(calls,fbo_attempts);
    jce_postfx_resize(pipeline,32,24);
    assert_taa_targets_empty(); assert_no_fake_owners();
    TEST_ASSERT_TRUE(ensure_taa_motion_fbo(pipeline));
    TEST_ASSERT_NOT_EQUAL_UINT16(before,pipeline->taa_motion_fb.idx);
    finish_pipeline_and_check();
}
static void test_motion_only_destroy_without_history(void)
{
    enable_motion_only(); TEST_ASSERT_TRUE(ensure_taa_motion_fbo(pipeline));
    TEST_ASSERT_FALSE(pipeline->taa_fbos_valid);
    finish_pipeline_and_check();
}
static void test_motion_validation_failure_rolls_back_before_retry(void)
{
    enable_motion_only(); reject_validation=true;
    TEST_ASSERT_FALSE(ensure_taa_motion_fbo(pipeline));
    assert_taa_targets_empty(); assert_no_fake_owners();
    reject_validation=false;
    TEST_ASSERT_TRUE(ensure_taa_motion_fbo(pipeline)); finish_pipeline_and_check();
}
static void test_motion_handle_exhaustion_rolls_back_before_retry(void)
{
    enable_motion_only(); failed_fbo_attempt=fbo_attempts+1;
    TEST_ASSERT_FALSE(ensure_taa_motion_fbo(pipeline));
    assert_taa_targets_empty(); assert_no_fake_owners();
    failed_fbo_attempt=0;
    TEST_ASSERT_TRUE(ensure_taa_motion_fbo(pipeline)); finish_pipeline_and_check();
}
static void test_motion_texture_failure_never_attempts_attachment(void)
{
    enable_motion_only(); failed_texture_attempt=texture_attempts+1;
    const unsigned calls = fbo_attempts;
    TEST_ASSERT_FALSE(ensure_taa_motion_fbo(pipeline));
    TEST_ASSERT_EQUAL_UINT32(calls,fbo_attempts);
    assert_taa_targets_empty(); assert_no_fake_owners();
    failed_texture_attempt=0;
    TEST_ASSERT_TRUE(ensure_taa_motion_fbo(pipeline)); finish_pipeline_and_check();
}
static void test_history_and_motion_toggle_resize_release_once(void)
{
    enable_motion_only(); TEST_ASSERT_TRUE(ensure_taa_motion_fbo(pipeline));
    const uint16_t motion = pipeline->taa_motion_fb.idx;
    jce_postfx_set_taa(pipeline,true,0.9f,1.0f,1.0f);
    create_taa_fbos(pipeline); TEST_ASSERT_TRUE(pipeline->taa_fbos_valid);
    TEST_ASSERT_EQUAL_UINT16(motion,pipeline->taa_motion_fb.idx);
    const unsigned calls = fbo_attempts;
    create_taa_fbos(pipeline); TEST_ASSERT_TRUE(ensure_taa_motion_fbo(pipeline));
    TEST_ASSERT_EQUAL_UINT32(calls,fbo_attempts);
    jce_postfx_set_taa(pipeline,false,0.9f,1.0f,1.0f);
    jce_postfx_resize(pipeline,64,40);
    assert_taa_targets_empty(); assert_no_fake_owners();
    /* Explicit repeated retirement is idempotent before public destruction. */
    destroy_taa_fbos(pipeline); assert_no_fake_owners();
    TEST_ASSERT_TRUE(ensure_taa_motion_fbo(pipeline));
    create_taa_fbos(pipeline); TEST_ASSERT_TRUE(pipeline->taa_fbos_valid);
    finish_pipeline_and_check();
}
static void test_history_failure_does_not_publish_valid_or_drop_motion(void)
{
    enable_motion_only(); TEST_ASSERT_TRUE(ensure_taa_motion_fbo(pipeline));
    const uint16_t motion = pipeline->taa_motion_fb.idx;
    failed_fbo_attempt=fbo_attempts+1;
    create_taa_fbos(pipeline);
    TEST_ASSERT_FALSE(pipeline->taa_fbos_valid);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX,pipeline->taa_history_tex.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX,pipeline->taa_history_fb.idx);
    TEST_ASSERT_EQUAL_UINT16(motion,pipeline->taa_motion_fb.idx);
    TEST_ASSERT_EQUAL_INT(0,textures[texture_count-1].refs);
    failed_fbo_attempt=0;
    create_taa_fbos(pipeline); TEST_ASSERT_TRUE(pipeline->taa_fbos_valid);
    finish_pipeline_and_check();
}
static void test_history_validation_and_texture_failure_close_partial_owner(void)
{
    create_pipeline(); reject_validation=true; create_taa_fbos(pipeline);
    assert_taa_targets_empty(); assert_no_fake_owners();
    reject_validation=false; failed_texture_attempt=texture_attempts+1;
    const unsigned calls = fbo_attempts; create_taa_fbos(pipeline);
    TEST_ASSERT_EQUAL_UINT32(calls,fbo_attempts);
    assert_taa_targets_empty(); assert_no_fake_owners();
    failed_texture_attempt=0; create_taa_fbos(pipeline);
    TEST_ASSERT_TRUE(pipeline->taa_fbos_valid); finish_pipeline_and_check();
}
/* Exercise the actual allocation owner and public query. No GPU work is
 * submitted for these controlled targets; publication mirrors apply's tail. */
static JcePostFXOutputTarget output_target(void)
{
    JcePostFXOutputTarget out;
    memset(&out, 0, sizeof out);
    out.struct_size = sizeof out;
    out.version = JCE_POSTFX_OUTPUT_TARGET_VERSION;
    return out;
}
static void check_motion_only_profile(JceGraphicsApiTier tier,
                                      bgfx_renderer_type_t backend, bool limited)
{
    /* The actual constructor selects policy. Its resources still use Noop. */
    profile_tier = tier;
    profile_backend = backend;
    override_profile = true;
    pipeline = jce_postfx_create(jce_allocator_default(), 1, 1);
    override_profile = false;
    TEST_ASSERT_NOT_NULL(pipeline);
    TEST_ASSERT_EQUAL_INT(limited, pipeline->limited_composite_sampling);
    jce_postfx_set_taa(pipeline, false, 0.9f, 1.0f, 1.0f);
    jce_postfx_set_motion_blur(pipeline, true, 1.0f);

    const jce_mat4 identity = jce_m4_identity();
    jce_postfx_set_taa_matrices(pipeline, &identity, &identity);
    const JceTextureHandle input = { 0 }, depth = { 1 };
    pipeline->shaders_loaded = true;
    pipeline->prog_motion_vec.idx = 0; /* A concrete program, not owned by the fixture. */

    /* An otherwise eligible blur reaches the first allocation. Reject it
     * there so even a missing guard cannot reach a real backend draw. */
    const unsigned textures_before = texture_attempts;
    const unsigned fbos_before = fbo_attempts;
    const unsigned submits_before = postfx_submissions;
    failed_texture_attempt = textures_before + 1;
    jce_postfx_apply(pipeline, input, depth);
    pipeline->prog_motion_vec.idx = UINT16_MAX;

    TEST_ASSERT_TRUE(jce_postfx_get_motion_blur(pipeline));
    TEST_ASSERT_FALSE(jce_postfx_get_motion_blur_active(pipeline));
    TEST_ASSERT_EQUAL_UINT32(textures_before + (limited ? 0u : 1u), texture_attempts);
    TEST_ASSERT_EQUAL_UINT32(fbos_before, fbo_attempts);
    TEST_ASSERT_EQUAL_UINT32(submits_before, postfx_submissions);
    assert_taa_targets_empty();
    JcePostFXOutputTarget out = output_target();
    if (limited) {
        TEST_ASSERT_TRUE(jce_postfx_get_output_target(pipeline, &out));
        TEST_ASSERT_EQUAL_UINT32(JCE_POSTFX_OUTPUT_BORROWED, out.kind);
        TEST_ASSERT_EQUAL_UINT16(input.idx, out.texture.idx);
        TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, out.framebuffer);
    } else {
        TEST_ASSERT_FALSE(jce_postfx_get_output_target(pipeline, &out));
    }
}

static void test_compatibility_desktop_gl_borrows_input_for_motion_only(void)
{
    check_motion_only_profile(JCE_GRAPHICS_API_TIER_COMPATIBILITY,
                              BGFX_RENDERER_TYPE_OPENGL, true);
}

static void test_compatibility_gles_keeps_motion_eligible(void)
{
    check_motion_only_profile(JCE_GRAPHICS_API_TIER_COMPATIBILITY,
                              BGFX_RENDERER_TYPE_OPENGLES, false);
}

static void test_stable_desktop_gl_keeps_motion_eligible(void)
{
    check_motion_only_profile(JCE_GRAPHICS_API_TIER_STABLE,
                              BGFX_RENDERER_TYPE_OPENGL, false);
}

static void publish_composite_slot(int slot)
{
    pipeline->output_ping = slot;
    pipeline->output_tex = pipeline->fbo_tex[slot];
    pipeline->output_fb = pipeline->fbo[slot];
}
static void test_output_target_requires_complete_actual_allocation(void)
{
    create_pipeline();
    JcePostFXOutputTarget out = output_target();
    TEST_ASSERT_FALSE(jce_postfx_get_output_target(pipeline, &out));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, out.texture.idx);
    pipeline->width = 65537u; pipeline->height = 24u;
    const uint32_t actual_format = postfx_color_format() == BGFX_TEXTURE_FORMAT_RGBA16F
        ? JCE_POSTFX_OUTPUT_FORMAT_RGBA16F : JCE_POSTFX_OUTPUT_FORMAT_RGBA8;
    create_fbos(pipeline); publish_composite_slot(0);
    TEST_ASSERT_TRUE(jce_postfx_get_output_target(pipeline, &out));
    TEST_ASSERT_EQUAL_UINT32(JCE_POSTFX_OUTPUT_OWNED, out.kind);
    TEST_ASSERT_EQUAL_UINT32(1, out.width);
    TEST_ASSERT_EQUAL_UINT32(24, out.height);
    TEST_ASSERT_EQUAL_UINT32(actual_format, out.format);
    /* A requested size change cannot rewrite the admitted allocation. */
    pipeline->width = 999u; pipeline->height = 888u;
    TEST_ASSERT_TRUE(jce_postfx_get_output_target(pipeline, &out));
    TEST_ASSERT_EQUAL_UINT32(1, out.width);
    TEST_ASSERT_EQUAL_UINT32(24, out.height);
    jce_postfx_resize(pipeline, 32, 18);
    TEST_ASSERT_FALSE(jce_postfx_get_output_target(pipeline, &out));
    TEST_ASSERT_EQUAL_UINT32(JCE_POSTFX_OUTPUT_UNAVAILABLE, out.kind);
    TEST_ASSERT_EQUAL_UINT32(0, pipeline->fbo_width[0]);
    TEST_ASSERT_EQUAL_UINT32(0, pipeline->fbo_height[0]);
    TEST_ASSERT_EQUAL_UINT32(JCE_POSTFX_OUTPUT_FORMAT_UNKNOWN, pipeline->fbo_format[0]);
    assert_no_fake_owners(); finish_pipeline_and_check();
}
static void test_output_target_borrowed_failed_and_versioned(void)
{
    create_pipeline();
    JcePostFXOutputTarget out = output_target();
    JceTextureHandle input = { 0 }, depth = { UINT16_MAX };
    jce_postfx_apply(pipeline, input, depth);
    TEST_ASSERT_TRUE(jce_postfx_get_output_target(pipeline, &out));
    TEST_ASSERT_EQUAL_UINT32(JCE_POSTFX_OUTPUT_BORROWED, out.kind);
    TEST_ASSERT_EQUAL_UINT16(0, out.texture.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, out.framebuffer);
    TEST_ASSERT_EQUAL_UINT32(0, out.width);
    TEST_ASSERT_EQUAL_UINT32(JCE_POSTFX_OUTPUT_FORMAT_UNKNOWN, out.format);
    out.version = 99; out.width = 777;
    TEST_ASSERT_FALSE(jce_postfx_get_output_target(pipeline, &out));
    TEST_ASSERT_EQUAL_UINT32(777, out.width);
    out = output_target(); --out.struct_size; out.width = 777;
    TEST_ASSERT_FALSE(jce_postfx_get_output_target(pipeline, &out));
    TEST_ASSERT_EQUAL_UINT32(777, out.width);
    TEST_ASSERT_FALSE(jce_postfx_get_output_target(pipeline, NULL));
    out = output_target();
    TEST_ASSERT_FALSE(jce_postfx_get_output_target(NULL, &out));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, out.texture.idx);
    jce_postfx_apply(pipeline, depth, depth);
    TEST_ASSERT_FALSE(jce_postfx_get_output_target(pipeline, &out));
}
static void test_output_target_zero_handles_and_mismatched_pair(void)
{
    create_pipeline(); create_fbos(pipeline); publish_composite_slot(0);
    const bgfx_texture_handle_t saved_tex = pipeline->fbo_tex[0];
    const bgfx_frame_buffer_handle_t saved_fb = pipeline->fbo[0];
    pipeline->fbo_tex[0].idx = pipeline->output_tex.idx = 0;
    pipeline->fbo[0].idx = pipeline->output_fb.idx = 0;
    JcePostFXOutputTarget out = output_target();
    TEST_ASSERT_TRUE(jce_postfx_get_output_target(pipeline, &out));
    TEST_ASSERT_EQUAL_UINT16(0, out.texture.idx);
    TEST_ASSERT_EQUAL_UINT16(0, out.framebuffer);
    pipeline->output_tex.idx = 88;
    TEST_ASSERT_FALSE(jce_postfx_get_output_target(pipeline, &out));
    pipeline->fbo_tex[0] = saved_tex; pipeline->fbo[0] = saved_fb;
    reset_output_state(pipeline); finish_pipeline_and_check();
}
static void test_output_target_partial_failure_closes_owner_and_metadata(void)
{
    create_pipeline(); reject_validation = true;
    create_fbos(pipeline);
    TEST_ASSERT_FALSE(pipeline->fbos_valid);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, pipeline->fbo_tex[0].idx);
    TEST_ASSERT_EQUAL_UINT32(0, pipeline->fbo_width[0]);
    assert_no_fake_owners();
    reject_validation = false; failed_fbo_attempt = fbo_attempts + 1;
    create_fbos(pipeline);
    TEST_ASSERT_FALSE(pipeline->fbos_valid); assert_no_fake_owners();
    failed_fbo_attempt = 0; failed_texture_attempt = texture_attempts + 1;
    const unsigned before_fbo = fbo_attempts;
    create_fbos(pipeline);
    TEST_ASSERT_FALSE(pipeline->fbos_valid);
    TEST_ASSERT_EQUAL_UINT32(before_fbo, fbo_attempts);
    TEST_ASSERT_EQUAL_UINT32(0, pipeline->fbo_height[0]); assert_no_fake_owners();
    failed_texture_attempt = 0; create_fbos(pipeline); publish_composite_slot(0);
    JcePostFXOutputTarget out = output_target();
    TEST_ASSERT_TRUE(jce_postfx_get_output_target(pipeline, &out));
    finish_pipeline_and_check();
}
static void test_output_target_per_slot_size_and_reuse(void)
{
    create_pipeline(); jce_postfx_resize(pipeline, 64, 40); create_fbos(pipeline);
    ensure_composite_fbo(pipeline, 1);
    const unsigned calls = fbo_attempts;
    ensure_composite_fbo(pipeline, 1);
    TEST_ASSERT_EQUAL_UINT32(calls, fbo_attempts);
    publish_composite_slot(1);
    JcePostFXOutputTarget out = output_target();
    TEST_ASSERT_TRUE(jce_postfx_get_output_target(pipeline, &out));
    TEST_ASSERT_EQUAL_UINT32(64, out.width); TEST_ASSERT_EQUAL_UINT32(40, out.height);
    ensure_bloom_fbos(pipeline); publish_composite_slot(2);
    TEST_ASSERT_TRUE(jce_postfx_get_output_target(pipeline, &out));
    TEST_ASSERT_EQUAL_UINT32(32, out.width); TEST_ASSERT_EQUAL_UINT32(20, out.height);
    destroy_fbos(pipeline);
    TEST_ASSERT_FALSE(jce_postfx_get_output_target(pipeline, &out));
    assert_no_fake_owners(); finish_pipeline_and_check();
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_output_target_per_slot_size_and_reuse);
    RUN_TEST(test_output_target_requires_complete_actual_allocation);
    RUN_TEST(test_output_target_borrowed_failed_and_versioned);
    RUN_TEST(test_output_target_zero_handles_and_mismatched_pair);
    RUN_TEST(test_output_target_partial_failure_closes_owner_and_metadata);
    RUN_TEST(test_compatibility_desktop_gl_borrows_input_for_motion_only);
    RUN_TEST(test_compatibility_gles_keeps_motion_eligible);
    RUN_TEST(test_stable_desktop_gl_keeps_motion_eligible);
    RUN_TEST(test_motion_only_resize_retires_then_recreates);
    RUN_TEST(test_motion_only_destroy_without_history);
    RUN_TEST(test_motion_validation_failure_rolls_back_before_retry);
    RUN_TEST(test_motion_handle_exhaustion_rolls_back_before_retry);
    RUN_TEST(test_motion_texture_failure_never_attempts_attachment);
    RUN_TEST(test_history_and_motion_toggle_resize_release_once);
    RUN_TEST(test_history_failure_does_not_publish_valid_or_drop_motion);
    RUN_TEST(test_history_validation_and_texture_failure_close_partial_owner);
    return UNITY_END();
}
