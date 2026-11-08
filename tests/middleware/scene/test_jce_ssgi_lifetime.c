/* Complete private SSGI retention owner with typed CPU dependencies. */
#include "unity.h"
#include "middleware/scene/jce_sr_internal.h"
#include <string.h>

static JceSceneRenderer owner;
static uint32_t frame;
static unsigned creates, destroys, resizes, frame_queries;
static bool fail_create, module_live;
static int module_token;
static int resized_width, resized_height;

static JceSsgi *ssgi_create_port(const JceSsgiDesc *desc)
{
    ++creates;
    TEST_ASSERT_EQUAL_PTR(owner.pak, desc->pak);
    TEST_ASSERT_TRUE(desc->width > 0 && desc->height > 0);
    if (fail_create) return NULL;
    TEST_ASSERT_FALSE(module_live);
    module_live = true;
    return (JceSsgi *)&module_token;
}

static void ssgi_destroy_port(JceSsgi *module)
{
    TEST_ASSERT_EQUAL_PTR(&module_token, module);
    TEST_ASSERT_TRUE_MESSAGE(module_live, "Each SSGI module is destroyed once");
    TEST_ASSERT_NULL(owner.ssgi);
    TEST_ASSERT_FALSE(owner.ssgi_active_frame);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, owner.ssgi_result_idx);
    TEST_ASSERT_FALSE(owner.ssgi_idle.valid);
    ++destroys;
    module_live = false;
}

static void ssgi_resize_port(JceSsgi *module, int width, int height)
{
    TEST_ASSERT_EQUAL_PTR(&module_token, module);
    TEST_ASSERT_TRUE(module_live);
    ++resizes;
    resized_width = width;
    resized_height = height;
}

static uint32_t renderer_frame_port(const JceRenderer *renderer)
{
    TEST_ASSERT_EQUAL_PTR(owner.renderer, renderer);
    ++frame_queries;
    return frame;
}

#define jce_ssgi_destroy ssgi_destroy_port
#define jce_renderer_get_frame_index renderer_frame_port
#include JCE_TEST_SSGI_LIFETIME_OWNER
#undef jce_renderer_get_frame_index
#undef jce_ssgi_destroy

static void render_call(bool reduced, bool requested, uint32_t sw, uint32_t sh)
{
    JceSceneRenderer *sr = &owner;
    const bool want_ssgi = !reduced && requested;
#define jce_ssgi_create ssgi_create_port
#define jce_ssgi_resize ssgi_resize_port
#include JCE_TEST_SSGI_FRAME_RESET
#include JCE_TEST_SSGI_PREPARE
#undef jce_ssgi_resize
#undef jce_ssgi_create
#include JCE_TEST_SSGI_RETIRE_WIRE
}

static void reset_call(void)
{
    JceSceneRenderer *sr = &owner;
#include JCE_TEST_SSGI_RESET_WIRE
}

static void destroy_call(void)
{
    JceSceneRenderer *sr = &owner;
#include JCE_TEST_SSGI_DESTROY_WIRE
}

void setUp(void)
{
    memset(&owner, 0, sizeof owner);
    owner.renderer = (JceRenderer *)&owner;
    owner.pak = (const JcePakArchive *)&module_token;
    owner.ssgi_result_idx = UINT16_MAX;
    frame = 1u;
    creates = destroys = resizes = frame_queries = 0u;
    fail_create = module_live = false;
    resized_width = resized_height = 0;
}

void tearDown(void)
{
    /* The actual release owner also keeps cleanup safe after Unity longjmp. */
    sr_ssgi_release(&owner);
    TEST_ASSERT_FALSE(module_live);
}

static void assert_idle_cleared(void)
{
    TEST_ASSERT_FALSE(owner.ssgi_idle.valid);
    TEST_ASSERT_FALSE(owner.ssgi_idle.wanted);
    TEST_ASSERT_EQUAL_UINT32(0u, owner.ssgi_idle.frames);
    TEST_ASSERT_EQUAL_UINT32(0u, owner.ssgi_idle.frame);
}

static void test_disabled_without_owner_does_not_query_or_allocate(void)
{
    for (unsigned i = 0; i < 256u; ++i) {
        ++frame;
        render_call(false, false, 640u, 480u);
    }
    TEST_ASSERT_EQUAL_UINT(0u, creates);
    TEST_ASSERT_EQUAL_UINT(0u, destroys);
    TEST_ASSERT_EQUAL_UINT(0u, frame_queries);
    assert_idle_cleared();
}

static void test_reduced_passes_preserve_pending_main_request_and_idle(void)
{
    render_call(false, true, 640u, 480u);
    frame = 2u;
    render_call(false, false, 640u, 480u);
    SrSsgiIdle before = owner.ssgi_idle;
    unsigned queries = frame_queries;
    for (unsigned i = 0u; i < 256u; ++i) {
        ++frame;
        render_call(true, false, 320u, 240u);
    }
    TEST_ASSERT_EQUAL_UINT32(before.frame, owner.ssgi_idle.frame);
    TEST_ASSERT_EQUAL_UINT32(before.frames, owner.ssgi_idle.frames);
    TEST_ASSERT_EQUAL(before.valid, owner.ssgi_idle.valid);
    TEST_ASSERT_EQUAL(before.wanted, owner.ssgi_idle.wanted);
    TEST_ASSERT_EQUAL_UINT(queries, frame_queries);
    TEST_ASSERT_EQUAL_UINT(1u, creates);
    TEST_ASSERT_EQUAL_UINT(0u, destroys);
    TEST_ASSERT_TRUE(module_live);
}

static void test_reduced_main_cycles_reuse_one_module_in_both_orders(void)
{
    render_call(false, true, 640u, 480u);
    for (frame = 2u; frame <= 401u; ++frame) {
        if (frame & 1u) render_call(true, false, 320u, 240u);
        render_call(false, true, 640u, 480u);
        if (!(frame & 1u)) render_call(true, false, 320u, 240u);
        TEST_ASSERT_TRUE(module_live);
    }
    TEST_ASSERT_EQUAL_UINT(1u, creates);
    TEST_ASSERT_EQUAL_UINT(0u, destroys);
    TEST_ASSERT_EQUAL_UINT(400u, resizes);
    TEST_ASSERT_TRUE(owner.ssgi_idle.wanted);
}

static void test_same_frame_views_or_requests_without_accumulating_idle(void)
{
    render_call(false, true, 640u, 480u);
    for (frame = 2u; frame <= 301u; ++frame) {
        render_call(false, false, 640u, 480u);
        render_call(false, true, 640u, 480u);
        for (unsigned i = 0u; i < 8u; ++i)
            render_call(false, false, 640u, 480u);
        TEST_ASSERT_TRUE(owner.ssgi_idle.wanted);
        TEST_ASSERT_EQUAL_UINT32(0u, owner.ssgi_idle.frames);
        TEST_ASSERT_TRUE(module_live);
    }
    TEST_ASSERT_EQUAL_UINT(1u, creates);
    TEST_ASSERT_EQUAL_UINT(0u, destroys);
}

static void test_repeated_false_views_do_not_count_as_cpu_frames(void)
{
    render_call(false, true, 640u, 480u);
    frame = 2u;
    for (unsigned i = 0u; i < 360u; ++i)
        render_call(false, false, 640u, 480u);
    TEST_ASSERT_EQUAL_UINT32(0u, owner.ssgi_idle.frames);
    TEST_ASSERT_TRUE(module_live);
    TEST_ASSERT_EQUAL_UINT(0u, destroys);
}

static void test_releases_after_120_complete_observed_unused_frames(void)
{
    render_call(false, true, 640u, 480u);
    for (frame = 2u; frame <= 121u; ++frame) {
        render_call(false, false, 640u, 480u);
        TEST_ASSERT_TRUE(module_live);
        TEST_ASSERT_EQUAL_UINT(0u, destroys);
    }
    frame = 122u;
    render_call(false, false, 640u, 480u);
    TEST_ASSERT_FALSE(module_live);
    TEST_ASSERT_NULL(owner.ssgi);
    TEST_ASSERT_EQUAL_UINT(1u, destroys);
    assert_idle_cleared();
    render_call(false, false, 640u, 480u);
    TEST_ASSERT_EQUAL_UINT(1u, destroys);
}

static void test_current_want_wins_at_release_boundary_and_restarts_grace(void)
{
    render_call(false, true, 640u, 480u);
    for (frame = 2u; frame <= 121u; ++frame)
        render_call(false, false, 640u, 480u);
    frame = 122u;
    render_call(false, true, 640u, 480u);
    TEST_ASSERT_EQUAL_UINT(1u, creates);
    TEST_ASSERT_EQUAL_UINT(0u, destroys);
    TEST_ASSERT_EQUAL_UINT32(0u, owner.ssgi_idle.frames);
    for (frame = 123u; frame <= 242u; ++frame) {
        render_call(false, false, 640u, 480u);
        TEST_ASSERT_TRUE(module_live);
    }
    frame = 243u;
    render_call(false, false, 640u, 480u);
    TEST_ASSERT_FALSE(module_live);
    TEST_ASSERT_EQUAL_UINT(1u, destroys);
}

static void test_gaps_reversal_and_uint32_wrap_reset_unproven_idle(void)
{
    render_call(false, true, 640u, 480u);
    for (frame = 2u; frame <= 100u; ++frame)
        render_call(false, false, 640u, 480u);
    TEST_ASSERT_EQUAL_UINT32(98u, owner.ssgi_idle.frames);
    const uint32_t discontinuities[] = {400u, 300u, UINT32_MAX, 0u};
    for (unsigned i = 0u; i < sizeof discontinuities / sizeof discontinuities[0]; ++i) {
        frame = discontinuities[i];
        render_call(false, false, 640u, 480u);
        TEST_ASSERT_EQUAL_UINT32(0u, owner.ssgi_idle.frames);
        TEST_ASSERT_TRUE(module_live);
    }
    frame = 1u;
    render_call(false, false, 640u, 480u);
    TEST_ASSERT_EQUAL_UINT32(1u, owner.ssgi_idle.frames);
    TEST_ASSERT_EQUAL_UINT(0u, destroys);
}

static void test_reset_and_destroy_clear_before_single_release(void)
{
    render_call(false, true, 640u, 480u);
    owner.ssgi_active_frame = true;
    owner.ssgi_result_idx = 7u;
    frame = 2u;
    render_call(false, false, 640u, 480u);
    reset_call();
    TEST_ASSERT_EQUAL_UINT(0u, destroys);
    TEST_ASSERT_TRUE(module_live);
    TEST_ASSERT_FALSE(owner.ssgi_active_frame);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, owner.ssgi_result_idx);
    assert_idle_cleared();
    reset_call();
    destroy_call();
    TEST_ASSERT_EQUAL_UINT(1u, destroys);
    render_call(false, true, 800u, 600u);
    TEST_ASSERT_EQUAL_UINT(2u, creates);
    owner.ssgi_active_frame = true;
    owner.ssgi_result_idx = 0u;
    destroy_call();
    TEST_ASSERT_EQUAL_UINT(2u, destroys);
    assert_idle_cleared();
}

static void test_repeated_entity_cache_resets_keep_non_entity_program(void)
{
    render_call(false, true, 640u, 480u);
    for (frame = 2u; frame <= 301u; ++frame) {
        reset_call();
        render_call(false, true, 640u, 480u);
        TEST_ASSERT_TRUE(module_live);
    }
    TEST_ASSERT_EQUAL_UINT(1u, creates);
    TEST_ASSERT_EQUAL_UINT(0u, destroys);
    TEST_ASSERT_EQUAL_UINT(300u, resizes);
}

static void test_failed_create_clears_activity_and_retries_without_idle_payload(void)
{
    fail_create = true;
    owner.ssgi_active_frame = true;
    render_call(false, true, 640u, 480u);
    TEST_ASSERT_EQUAL_UINT(1u, creates);
    TEST_ASSERT_FALSE(owner.ssgi_active_frame);
    TEST_ASSERT_NULL(owner.ssgi);
    TEST_ASSERT_EQUAL_UINT(0u, destroys);
    TEST_ASSERT_EQUAL_UINT(0u, frame_queries);
    assert_idle_cleared();
    fail_create = false;
    ++frame;
    render_call(false, true, 640u, 480u);
    TEST_ASSERT_TRUE(module_live);
    TEST_ASSERT_EQUAL_UINT(2u, creates);
    TEST_ASSERT_EQUAL_UINT(0u, destroys);
}

static void test_resize_reuses_module_and_reduced_views_never_resize_it(void)
{
    render_call(false, true, 640u, 480u);
    ++frame;
    render_call(true, false, 320u, 240u);
    TEST_ASSERT_EQUAL_UINT(0u, resizes);
    render_call(false, true, 1024u, 768u);
    TEST_ASSERT_EQUAL_UINT(1u, resizes);
    TEST_ASSERT_EQUAL_INT(1024, resized_width);
    TEST_ASSERT_EQUAL_INT(768, resized_height);
    TEST_ASSERT_EQUAL_UINT(1u, creates);
    TEST_ASSERT_EQUAL_UINT(0u, destroys);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_disabled_without_owner_does_not_query_or_allocate);
    RUN_TEST(test_reduced_passes_preserve_pending_main_request_and_idle);
    RUN_TEST(test_reduced_main_cycles_reuse_one_module_in_both_orders);
    RUN_TEST(test_same_frame_views_or_requests_without_accumulating_idle);
    RUN_TEST(test_repeated_false_views_do_not_count_as_cpu_frames);
    RUN_TEST(test_releases_after_120_complete_observed_unused_frames);
    RUN_TEST(test_current_want_wins_at_release_boundary_and_restarts_grace);
    RUN_TEST(test_gaps_reversal_and_uint32_wrap_reset_unproven_idle);
    RUN_TEST(test_reset_and_destroy_clear_before_single_release);
    RUN_TEST(test_repeated_entity_cache_resets_keep_non_entity_program);
    RUN_TEST(test_failed_create_clears_activity_and_retries_without_idle_payload);
    RUN_TEST(test_resize_reuses_module_and_reduced_views_never_resize_it);
    return UNITY_END();
}
