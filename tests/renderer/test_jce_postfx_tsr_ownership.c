/* Exact production PostFX TU with controlled RT ownership requests.
 * Ordinary constructor resources use bgfx Noop, never a window or GPU.
 * Pinned bgfx_p.h attachment refs: validation rejection retains the caller
 * owner; post-validation handle exhaustion still honors destroyTextures.
 * CPU request closure only, not GPU free/fence or real vendor allocator OOM.
 */
#include "unity.h"
#include <bgfx/c99/bgfx.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/os/core/jce_trace.h>
#include <string.h>

#define FAKE_BASE 1024u
#define FAKE_MAX 128u
#define FAKE_FB_BASE 4096u
#define FAKE_ATTACH_MAX 8u

typedef struct FakeTexture {
    int refs;
    bool issued, owned;
    unsigned destroy_requests;
} FakeTexture;
typedef struct FakeFrameBuffer {
    bool issued, destroyed;
    uint8_t count;
    uint16_t texture[FAKE_ATTACH_MAX];
} FakeFrameBuffer;
static FakeTexture textures[FAKE_MAX];
static FakeFrameBuffer framebuffers[FAKE_MAX];
static unsigned texture_count, framebuffer_count, texture_attempts, fbo_attempts;
static unsigned bad_releases, failed_texture_attempt, failed_fbo_attempt;
static bool reject_validation;
static JceRenderer *renderer;
static JcePostFXPipeline *pipeline;

static FakeTexture *fake_texture(uint16_t idx)
{
    if (idx < FAKE_BASE || idx >= FAKE_BASE + texture_count) return NULL;
    return &textures[idx - FAKE_BASE];
}
static void fake_dec(uint16_t idx)
{
    FakeTexture *t = fake_texture(idx);
    if (!t || !t->issued || t->refs <= 0) { ++bad_releases; return; }
    if (--t->refs == 0) ++t->destroy_requests;
}
static bgfx_texture_handle_t test_create_texture(uint16_t w, uint16_t h,
    bool mips, uint16_t layers, bgfx_texture_format_t format, uint64_t flags,
    const bgfx_memory_t *memory, uint64_t external)
{
    bgfx_texture_handle_t result = { UINT16_MAX };
    (void)w; (void)h; (void)mips; (void)layers; (void)format; (void)flags;
    (void)memory; (void)external;
    ++texture_attempts;
    if (texture_attempts == failed_texture_attempt || texture_count == FAKE_MAX)
        return result;
    result.idx = (uint16_t)(FAKE_BASE + texture_count);
    textures[texture_count].refs = 1;
    textures[texture_count].issued = true;
    ++texture_count;
    return result;
}
static bgfx_frame_buffer_handle_t test_create_fbo(uint8_t count,
    const bgfx_attachment_t *attachments, bool take_ownership)
{
    bgfx_frame_buffer_handle_t result = { UINT16_MAX };
    ++fbo_attempts;
    /* Pinned validation returns before attachment ref/ownership mutation. */
    if (reject_validation || !count || count > FAKE_ATTACH_MAX) return result;
    for (uint8_t i = 0; i < count; ++i)
        if (!fake_texture(attachments[i].handle.idx)) return result;
    bool admitted = fbo_attempts != failed_fbo_attempt && framebuffer_count < FAKE_MAX;
    if (admitted) {
        FakeFrameBuffer *f = &framebuffers[framebuffer_count];
        result.idx = (uint16_t)(FAKE_FB_BASE + framebuffer_count++);
        f->issued = true; f->count = count;
        for (uint8_t i = 0; i < count; ++i) {
            f->texture[i] = attachments[i].handle.idx;
            ++fake_texture(f->texture[i])->refs;
        }
    }
    /* In pinned BGFX this is AFTER the admitted-handle block: allocation
     * exhaustion still takes ownership; do not model it as validation reject. */
    if (take_ownership) {
        for (uint8_t i = 0; i < count; ++i) {
            FakeTexture *t = fake_texture(attachments[i].handle.idx);
            if (!t->owned) { t->owned = true; fake_dec(attachments[i].handle.idx); }
        }
    }
    return result;
}
static bgfx_frame_buffer_handle_t test_create_fbo_from_handles(uint8_t count,
    const bgfx_texture_handle_t *handles, bool take_ownership)
{
    bgfx_frame_buffer_handle_t invalid = { UINT16_MAX };
    bgfx_attachment_t attachments[FAKE_ATTACH_MAX];
    if (count > FAKE_ATTACH_MAX) return invalid;
    memset(attachments, 0, sizeof attachments);
    /* AE textures have no mips: the pinned handles overload uses NONE. */
    for (uint8_t i = 0; i < count; ++i)
        bgfx_attachment_init(&attachments[i], handles[i], BGFX_ACCESS_WRITE,
                             0, 1, 0, BGFX_RESOLVE_NONE);
    return test_create_fbo(count, attachments, take_ownership);
}
static void test_destroy_fbo(bgfx_frame_buffer_handle_t handle)
{
    if (handle.idx < FAKE_FB_BASE || handle.idx >= FAKE_FB_BASE + framebuffer_count) {
        bgfx_destroy_frame_buffer(handle); return;
    }
    FakeFrameBuffer *f = &framebuffers[handle.idx - FAKE_FB_BASE];
    if (!f->issued || f->destroyed) { ++bad_releases; return; }
    f->destroyed = true;
    for (uint8_t i = 0; i < f->count; ++i) fake_dec(f->texture[i]);
}
static void test_destroy_texture(bgfx_texture_handle_t handle)
{
    if (fake_texture(handle.idx)) fake_dec(handle.idx);
    else bgfx_destroy_texture(handle); /* Real Noop neutral 3D LUT, etc. */
}

#define bgfx_create_texture_2d test_create_texture
#define bgfx_create_frame_buffer_from_attachment test_create_fbo
#define bgfx_create_frame_buffer_from_handles test_create_fbo_from_handles
#define bgfx_destroy_frame_buffer test_destroy_fbo
#define bgfx_destroy_texture test_destroy_texture
#ifndef JCE_TEST_POSTFX_OWNER
#define JCE_TEST_POSTFX_OWNER "renderer/jce_postfx.c"
#endif
#include JCE_TEST_POSTFX_OWNER
#undef bgfx_destroy_texture
#undef bgfx_destroy_frame_buffer
#undef bgfx_create_frame_buffer_from_handles
#undef bgfx_create_frame_buffer_from_attachment
#undef bgfx_create_texture_2d

static void clear_fake(void)
{
    memset(textures, 0, sizeof textures); memset(framebuffers, 0, sizeof framebuffers);
    texture_count = framebuffer_count = texture_attempts = fbo_attempts = 0;
    bad_releases = failed_texture_attempt = failed_fbo_attempt = 0;
    reject_validation = false;
}
void setUp(void)
{
    clear_fake(); pipeline = NULL;
    jce_trace_set_enabled(false);
    renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(renderer);
}
void tearDown(void)
{
    if (pipeline) jce_postfx_destroy(pipeline);
    pipeline = NULL;
    jce_renderer_destroy(renderer); renderer = NULL;
}
static void create_pipeline(void)
{
    pipeline = jce_postfx_create(jce_allocator_default(), 1, 1);
    TEST_ASSERT_NOT_NULL(pipeline);
}
static void finish_pipeline_and_check(void)
{
    jce_postfx_destroy(pipeline); pipeline = NULL;
    TEST_ASSERT_EQUAL_UINT32(0, bad_releases);
    for (unsigned i = 0; i < texture_count; ++i) {
        TEST_ASSERT_EQUAL_INT(0, textures[i].refs);
        TEST_ASSERT_EQUAL_UINT32(1, textures[i].destroy_requests);
    }
    for (unsigned i = 0; i < framebuffer_count; ++i)
        TEST_ASSERT_TRUE(framebuffers[i].destroyed);
}
#ifndef JCE_POSTFX_OWNERSHIP_SUPPORT_ONLY
static void test_pinned_provider_ownership_positive(void)
{
    bgfx_texture_handle_t t = test_create_texture(64,64,false,1,BGFX_TEXTURE_FORMAT_RGBA8,0,NULL,0);
    bgfx_attachment_t a; memset(&a,0,sizeof a); a.handle=t;
    bgfx_frame_buffer_handle_t f = test_create_fbo(1,&a,true);
    TEST_ASSERT_EQUAL_INT(1,fake_texture(t.idx)->refs);
    test_destroy_fbo(f);
    TEST_ASSERT_EQUAL_INT(0,fake_texture(t.idx)->refs);
    TEST_ASSERT_EQUAL_UINT32(1,fake_texture(t.idx)->destroy_requests);
    TEST_ASSERT_EQUAL_UINT32(0,bad_releases);
}
static void test_actual_history_destroy_releases_owned_attachment_once(void)
{
    create_pipeline(); TEST_ASSERT_TRUE(ensure_tsr_buffers(pipeline,64,64));
    finish_pipeline_and_check();
}
static void test_actual_history_reuse_resize_destroy(void)
{
    create_pipeline(); TEST_ASSERT_TRUE(ensure_tsr_buffers(pipeline,64,64));
    unsigned requests=fbo_attempts;
    TEST_ASSERT_TRUE(ensure_tsr_buffers(pipeline,64,64));
    TEST_ASSERT_EQUAL_UINT32(requests,fbo_attempts);
    jce_postfx_resize(pipeline,48,48); /* Public resize alone does not resize TSR. */
    TEST_ASSERT_TRUE(ensure_tsr_buffers(pipeline,96,80));
    finish_pipeline_and_check();
}
static void test_actual_motion_reuse_resize_destroy(void)
{
    create_pipeline(); TEST_ASSERT_TRUE(ensure_tsr_motion(pipeline,32,32));
    unsigned requests=fbo_attempts;
    TEST_ASSERT_TRUE(ensure_tsr_motion(pipeline,32,32));
    TEST_ASSERT_EQUAL_UINT32(requests,fbo_attempts);
    TEST_ASSERT_TRUE(ensure_tsr_motion(pipeline,48,40));
    finish_pipeline_and_check();
}
static void test_validation_failure_keeps_caller_texture_owner(void)
{
    create_pipeline(); reject_validation=true;
    TEST_ASSERT_FALSE(ensure_tsr_buffers(pipeline,64,64));
    reject_validation=false; finish_pipeline_and_check();
}
static void test_handle_exhaustion_obeys_pinned_ownership(void)
{
    create_pipeline(); failed_fbo_attempt=fbo_attempts+1;
    TEST_ASSERT_FALSE(ensure_tsr_buffers(pipeline,64,64));
    finish_pipeline_and_check();
}
static void test_second_fbo_failure_closes_partial_history(void)
{
    create_pipeline(); failed_fbo_attempt=fbo_attempts+2;
    TEST_ASSERT_FALSE(ensure_tsr_buffers(pipeline,64,64));
    finish_pipeline_and_check();
}
static void test_second_texture_failure_closes_partial_history(void)
{
    create_pipeline(); failed_texture_attempt=texture_attempts+2;
    TEST_ASSERT_FALSE(ensure_tsr_buffers(pipeline,64,64));
    finish_pipeline_and_check();
}
static void test_partial_history_failure_can_retry_before_destruction(void)
{
    create_pipeline();
    failed_fbo_attempt = fbo_attempts + 2;
    TEST_ASSERT_FALSE(ensure_tsr_buffers(pipeline, 64, 64));
    failed_fbo_attempt = 0;
    TEST_ASSERT_TRUE(ensure_tsr_buffers(pipeline, 64, 64));
    finish_pipeline_and_check();
}
static void test_motion_handle_exhaustion_can_retry_before_destruction(void)
{
    create_pipeline();
    failed_fbo_attempt = fbo_attempts + 1;
    TEST_ASSERT_FALSE(ensure_tsr_motion(pipeline, 32, 32));
    failed_fbo_attempt = 0;
    TEST_ASSERT_TRUE(ensure_tsr_motion(pipeline, 32, 32));
    finish_pipeline_and_check();
}
static void enable_ae_provider_contract(void)
{
    create_pipeline();
    /* Admit the CPU ownership provider, not Noop caps or hardware support. */
    pipeline->ae_support = 1;
}
static void assert_ae_targets_cleared(void)
{
    TEST_ASSERT_FALSE(pipeline->ae_targets_valid);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, pipeline->ae_small_fb.idx);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, pipeline->ae_small_tex.idx);
    for (int i = 0; i < AE_RING; ++i) {
        TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, pipeline->ae_staging[i].idx);
        TEST_ASSERT_NULL(pipeline->ae_pixels[i]);
        TEST_ASSERT_FALSE(pipeline->ae_inflight[i]);
        TEST_ASSERT_EQUAL_UINT32(0, pipeline->ae_ready[i]);
    }
}
static void test_actual_ae_target_reuse_destroy(void)
{
    enable_ae_provider_contract();
    unsigned texture_before = texture_attempts, fbo_before = fbo_attempts;
    TEST_ASSERT_TRUE(ae_ensure_targets(pipeline));
    TEST_ASSERT_EQUAL_UINT32(texture_before + 1 + AE_RING, texture_attempts);
    TEST_ASSERT_EQUAL_UINT32(fbo_before + 1, fbo_attempts);
    TEST_ASSERT_TRUE(ae_ensure_targets(pipeline));
    TEST_ASSERT_EQUAL_UINT32(texture_before + 1 + AE_RING, texture_attempts);
    TEST_ASSERT_EQUAL_UINT32(fbo_before + 1, fbo_attempts);
    ae_destroy_targets(pipeline);
    assert_ae_targets_cleared();
    finish_pipeline_and_check();
}
static void test_ae_validation_failure_can_retry_before_destruction(void)
{
    enable_ae_provider_contract();
    reject_validation = true;
    TEST_ASSERT_FALSE(ae_ensure_targets(pipeline));
    assert_ae_targets_cleared();
    reject_validation = false;
    TEST_ASSERT_TRUE(ae_ensure_targets(pipeline));
    finish_pipeline_and_check();
}
static void test_ae_handle_exhaustion_can_retry_before_destruction(void)
{
    enable_ae_provider_contract();
    failed_fbo_attempt = fbo_attempts + 1;
    TEST_ASSERT_FALSE(ae_ensure_targets(pipeline));
    assert_ae_targets_cleared();
    failed_fbo_attempt = 0;
    TEST_ASSERT_TRUE(ae_ensure_targets(pipeline));
    finish_pipeline_and_check();
}
static void test_ae_partial_staging_failure_can_retry_before_destruction(void)
{
    enable_ae_provider_contract();
    /* Fail the second staging texture, after the RT and first ring entry. */
    failed_texture_attempt = texture_attempts + 3;
    TEST_ASSERT_FALSE(ae_ensure_targets(pipeline));
    assert_ae_targets_cleared();
    failed_texture_attempt = 0;
    TEST_ASSERT_TRUE(ae_ensure_targets(pipeline));
    finish_pipeline_and_check();
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pinned_provider_ownership_positive);
    RUN_TEST(test_actual_history_destroy_releases_owned_attachment_once);
    RUN_TEST(test_actual_history_reuse_resize_destroy);
    RUN_TEST(test_actual_motion_reuse_resize_destroy);
    RUN_TEST(test_validation_failure_keeps_caller_texture_owner);
    RUN_TEST(test_handle_exhaustion_obeys_pinned_ownership);
    RUN_TEST(test_second_fbo_failure_closes_partial_history);
    RUN_TEST(test_second_texture_failure_closes_partial_history);
    RUN_TEST(test_partial_history_failure_can_retry_before_destruction);
    RUN_TEST(test_motion_handle_exhaustion_can_retry_before_destruction);
    RUN_TEST(test_actual_ae_target_reuse_destroy);
    RUN_TEST(test_ae_validation_failure_can_retry_before_destruction);
    RUN_TEST(test_ae_handle_exhaustion_can_retry_before_destruction);
    RUN_TEST(test_ae_partial_staging_failure_can_retry_before_destruction);
    return UNITY_END();
}
#endif /* JCE_POSTFX_OWNERSHIP_SUPPORT_ONLY */
