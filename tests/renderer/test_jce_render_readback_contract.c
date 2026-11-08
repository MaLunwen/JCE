#include <jce/renderer/jce_render_readback.h>

#include <unity.h>

#include <stdint.h>
#include <stdlib.h>
#include <jce/renderer/jce_shaders.h>
#include <string.h>

/* Exact production TU with counted backend ports. These prove ownership and
 * asynchronous destination lifetime, not GPU transfer or platform pixels. */
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_lowlevel.h>
#include <jce/os/core/jce_alloc.h>
#include "os/core/jce_memory.h"
#include <bgfx/c99/bgfx.h>
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>
#include "os/core/jce_trace_scope_internal.h"
#include "renderer/jce_renderer_submission_trace_internal.h"
#include "renderer/jce_instance_upload.h"
#include "renderer/jce_renderer_lifecycle_internal.h"
#include "renderer/jce_renderer_caps_internal.h"
#include "renderer/jce_render_readback_internal.h"
#include "renderer/jce_offscreen_retirement_internal.h"
#include "renderer/jce_upload_metrics.h"
#include "renderer/jce_bgfx_allocator_trace_internal.h"
#ifdef JCE_TEST_FRAME_SERIAL_HEADER
#include JCE_TEST_FRAME_SERIAL_HEADER
#endif
#include "renderer/jce_renderer_frame_serial_internal.h"
struct SDL_Renderer;
struct JceRenderer {
    bool is_fallback, headless;
    JceInstanceUploadOwner instance_upload;
    JceSubmittedPayloadStats submitted_payload;
    struct SDL_Renderer *sdl_renderer;
    bgfx_uniform_handle_t u_sdf_params, u_sdf_outline;
    bgfx_uniform_handle_t u_sdf_shadow_offset, u_sdf_shadow_color;
    bgfx_uniform_handle_t u_tex_color, u_light_dir, u_light_color;
};
static JceRenderer renderer_token;
static struct {
    uint32_t frame_index, offscreen_submission_index;
    JceUploadMetricsBridge payload;
    JceBgfxAllocatorTrace allocation_trace;
} s_device;
static unsigned renderer_frees;
static unsigned retirement_calls,instance_calls;
static bool no_advance,backend_joined;

static bgfx_caps_t caps;
static uint32_t current_frame, ready_frame;
static unsigned texture_creates, texture_destroys, pixel_frees, all_frees, read_calls;
static bool fail_texture, borrowed,premature_cpu_release;
static void *destination;

static const bgfx_caps_t *controlled_caps(void) { return &caps; }
bool jce_renderer_caps_api_web_context_current(void) { return true; }

static bgfx_texture_handle_t controlled_create_texture(uint16_t w, uint16_t h,
    bool mips, uint16_t layers, bgfx_texture_format_t format, uint64_t flags,
    const bgfx_memory_t *memory, uint16_t skip)
{
    (void)w; (void)h; (void)mips; (void)layers; (void)format;
    (void)flags; (void)memory; (void)skip;
    ++texture_creates;
    return (bgfx_texture_handle_t){fail_texture ? UINT16_MAX : 0};
}
static uint32_t controlled_read(bgfx_texture_handle_t texture, void *pixels,
                                uint8_t mip, uint8_t layer)
{
    (void)mip; (void)layer;
    TEST_ASSERT_EQUAL_UINT16(0, texture.idx);
    TEST_ASSERT_NOT_NULL(pixels);
    destination = pixels;
    borrowed = true;
    ++read_calls;
    return ready_frame;
}
static void complete_at(uint32_t frame)
{
    current_frame = frame;s_device.frame_index=frame;
    if (borrowed && frame == ready_frame) {
        memset(destination, 0x5a, 16); /* The actual submitted CPU destination. */
        borrowed = false;
    }
}
static uint32_t controlled_frame(bool capture)
{
    (void)capture;
    uint32_t published=s_device.frame_index;
    if(!no_advance) complete_at(current_frame + 1u);
    s_device.frame_index=published;
    return current_frame;
}
static void controlled_destroy(bgfx_texture_handle_t texture)
{
    TEST_ASSERT_EQUAL_UINT16(0, texture.idx);
    /* Retiring a handle is ordered backend work; its CPU borrow is separate. */
    ++texture_destroys;
}
static void controlled_free(void *data)
{
    if (!data) return;
    if (data == &renderer_token) {
        /* The fixture token is static; count the actual final owner release. */
        ++renderer_frees;
        return;
    }
    if (data == destination) {
        /* Record violation before releasing this controlled CPU allocation.
         * Historical controls must fail ordinary assertions, never a delayed
         * simulator write through freed storage or a teardown longjmp leak. */
        if(borrowed && !backend_joined)premature_cpu_release=true;
        ++pixel_frees;
        destination = NULL;borrowed=false;
    }
    ++all_frees;
    jce_free(data);
}
#undef JCE_FREE
#define JCE_FREE controlled_free
#define jce_gfx_texture_valid(t) ((t).idx != UINT16_MAX)
#define jce_renderer_get_frame_index(...) (s_device.frame_index)
#define bgfx_get_caps controlled_caps
#define bgfx_create_texture_2d controlled_create_texture
#define bgfx_read_texture controlled_read
#define bgfx_blit(...) ((void)0)
#define bgfx_destroy_texture controlled_destroy
#define bgfx_frame controlled_frame
static void counted_retirement(const JceRenderer *renderer,uint32_t ordinal)
{TEST_ASSERT_EQUAL_PTR(&renderer_token,renderer);TEST_ASSERT_EQUAL_UINT32(s_device.offscreen_submission_index,ordinal);++retirement_calls;}
static void counted_instance(JceInstanceUploadOwner *owner,uint32_t ordinal)
{TEST_ASSERT_EQUAL_PTR(&renderer_token.instance_upload,owner);TEST_ASSERT_EQUAL_UINT32(s_device.offscreen_submission_index,ordinal);++instance_calls;}
#define jce_offscreen_retirement_collect counted_retirement
#define jce_instance_upload_owner_advance counted_instance
#include JCE_TEST_READBACK_SUBMISSION_OWNER
#ifndef JCE_TEST_READBACK_OWNER
#define JCE_TEST_READBACK_OWNER "renderer/jce_render_readback.c"
#endif
#include JCE_TEST_READBACK_OWNER

/* Consume the complete actual destroy owner. Only unrelated dependencies and
 * backend boundaries have typed CPU ports; no cleanup sequence is copied. */
static bool s_dbg_text_enabled;
static unsigned destroy_order, destroy_before_at, destroy_join_at, destroy_after_at;
static unsigned destroy_join_calls, destroy_before_calls, destroy_after_calls;
static unsigned destroy_unbind_calls, destroy_offscreen_calls, destroy_upload_calls;
static bool destroy_service_closed_at_join, destroy_destination_held_at_join;
static bool destroy_queue_detached_at_join;
static JceRenderReadbackQueue *destroy_expected_queue;
static JceUploadMetricsBridge *destroy_seen_bridge;
static JceSubmittedPayloadStats *destroy_seen_payload;
static const JceRenderer *destroy_seen_offscreen;
static JceInstanceUploadOwner *destroy_seen_upload;

static void counted_upload_unbind(JceUploadMetricsBridge *bridge,
    JceSubmittedPayloadStats *payload)
{
    ++destroy_unbind_calls;
    destroy_seen_bridge = bridge;
    destroy_seen_payload = payload;
}
static void counted_capture_shutdown(void) { }
static void counted_before_backend_shutdown(JceRenderer *renderer)
{
    ++destroy_before_calls;
    destroy_before_at = ++destroy_order;
    jce_render_readback_before_backend_shutdown(renderer);
}
static void counted_after_backend_shutdown(JceRenderer *renderer)
{
    ++destroy_after_calls;
    destroy_after_at = ++destroy_order;
    jce_render_readback_after_backend_shutdown(renderer);
}
static void counted_backend_shutdown(void)
{
    /* Record violations rather than longjmp before the test frees its wrapper.
     * Never simulate a backend write after a negative freed the destination. */
    ++destroy_join_calls;
    destroy_join_at = ++destroy_order;
    destroy_service_closed_at_join = s_readback_renderer == NULL;
    destroy_destination_held_at_join = borrowed && destination != NULL &&
        pixel_frees == 0 && s_readback_cpu_buffers == 1;
    destroy_queue_detached_at_join = destroy_expected_queue != NULL &&
        destroy_expected_queue->closing && destroy_expected_queue->renderer == NULL &&
        destroy_expected_queue->backend_owner == &renderer_token;
    backend_joined = true; /* The sole controlled backend-return boundary. */
}
static void counted_offscreen_shutdown(const JceRenderer *renderer)
{
    ++destroy_offscreen_calls;
    destroy_seen_offscreen = renderer;
}
static void counted_upload_destroy(JceInstanceUploadOwner *owner)
{
    ++destroy_upload_calls;
    destroy_seen_upload = owner;
}
static JceShaderSet counted_owned_shader_set(const JceRenderer *renderer)
{
    JceShaderSet owned;
    (void)renderer;
    memset(&owned, 0, sizeof owned);
    return owned;
}
static void counted_release_shader_set(const JceShaderSet *owned) { (void)owned; }
static void counted_uniform_destroy(bgfx_uniform_handle_t uniform) { (void)uniform; }
static const bgfx_stats_t *counted_shutdown_stats(void) { return NULL; }
static char *counted_shutdown_getenv(const char *name) { (void)name; return NULL; }
static void counted_shutdown_dependency(void) { }
static void counted_shader_dev_release(JceRenderer *renderer) { (void)renderer; }
static void counted_sdl_renderer_destroy(struct SDL_Renderer *renderer) { (void)renderer; }
static void counted_allocator_trace_end(JceBgfxAllocatorTrace *trace, uint32_t reason)
{ (void)trace; (void)reason; }

#define renderer_shader_dev_release counted_shader_dev_release
#define jce_upload_metrics_unbind counted_upload_unbind
#define jce_renderer_readback_capture_shutdown counted_capture_shutdown
#define jce_render_readback_before_backend_shutdown counted_before_backend_shutdown
#define jce_render_readback_after_backend_shutdown counted_after_backend_shutdown
#define jce_offscreen_retirement_shutdown counted_offscreen_shutdown
#define jce_instance_upload_owner_destroy counted_upload_destroy
#define renderer_owned_shader_set counted_owned_shader_set
#define jce_shader_set_release_owned counted_release_shader_set
#define bgfx_destroy_uniform counted_uniform_destroy
#define bgfx_get_stats counted_shutdown_stats
#define getenv counted_shutdown_getenv
#define bgfx_shutdown counted_backend_shutdown
#define SDL_DestroyRenderer counted_sdl_renderer_destroy
#define jce_text_shutdown counted_shutdown_dependency
#define jce_pbr_material_shutdown counted_shutdown_dependency
#define jce_lighting_system_shutdown counted_shutdown_dependency
#define jce_ies_cache_shutdown counted_shutdown_dependency
#define jce_impostor_shutdown counted_shutdown_dependency
#define jce_shader_deferred_shutdown counted_shutdown_dependency
#define jce_bgfx_trace_end counted_allocator_trace_end
#define jce_impostor_after_backend_shutdown counted_shutdown_dependency
#define jce_rcb_after_backend_shutdown counted_shutdown_dependency
#define gl_ladder_release counted_shutdown_dependency
#define jce_renderer_caps_api_reset counted_shutdown_dependency
#undef LOG_INFO
#define LOG_INFO(...) ((void)0)
#include JCE_TEST_READBACK_DESTROY_OWNER
#undef renderer_shader_dev_release
#undef jce_upload_metrics_unbind
#undef jce_renderer_readback_capture_shutdown
#undef jce_render_readback_before_backend_shutdown
#undef jce_render_readback_after_backend_shutdown
#undef jce_offscreen_retirement_shutdown
#undef jce_instance_upload_owner_destroy
#undef renderer_owned_shader_set
#undef jce_shader_set_release_owned
#undef bgfx_destroy_uniform
#undef bgfx_get_stats
#undef getenv
#undef bgfx_shutdown
#undef SDL_DestroyRenderer
#undef jce_text_shutdown
#undef jce_pbr_material_shutdown
#undef jce_lighting_system_shutdown
#undef jce_ies_cache_shutdown
#undef jce_impostor_shutdown
#undef jce_shader_deferred_shutdown
#undef jce_bgfx_trace_end
#undef jce_impostor_after_backend_shutdown
#undef jce_rcb_after_backend_shutdown
#undef gl_ladder_release
#undef jce_renderer_caps_api_reset
#undef LOG_INFO

static uint64_t trace_context=UINT64_C(9007199254740993);
bool jce_renderer_gpu_observation_context_internal(uint64_t *epoch,uint32_t *frame)
{
    if(epoch)*epoch=trace_context;
    if(frame)*frame=s_device.frame_index;
    return trace_context!=0;
}

void setUp(void)
{
    memset(&caps, 0, sizeof caps);
    caps.supported = BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK;
    caps.formats[BGFX_TEXTURE_FORMAT_RGBA8] = BGFX_CAPS_FORMAT_TEXTURE_2D;
    current_frame = 0; ready_frame = 6;
    trace_context=UINT64_C(9007199254740993);
    memset(&s_device,0,sizeof s_device);memset(&renderer_token,0,sizeof renderer_token);
    retirement_calls=instance_calls=0;no_advance=backend_joined=false;
    texture_creates = texture_destroys = pixel_frees = all_frees = read_calls = 0;
    fail_texture = borrowed = premature_cpu_release = false; destination = NULL;
    renderer_frees = 0;
    destroy_order = destroy_before_at = destroy_join_at = destroy_after_at = 0;
    destroy_join_calls = destroy_before_calls = destroy_after_calls = 0;
    destroy_unbind_calls = destroy_offscreen_calls = destroy_upload_calls = 0;
    destroy_service_closed_at_join = destroy_destination_held_at_join = false;
    destroy_queue_detached_at_join = false;
    destroy_expected_queue = NULL;
    destroy_seen_bridge = NULL; destroy_seen_payload = NULL;
    destroy_seen_offscreen = NULL; destroy_seen_upload = NULL;
    bgfx_uniform_handle_t *uniforms[] = {
        &renderer_token.u_sdf_params, &renderer_token.u_sdf_outline,
        &renderer_token.u_sdf_shadow_offset, &renderer_token.u_sdf_shadow_color,
        &renderer_token.u_tex_color, &renderer_token.u_light_dir,
        &renderer_token.u_light_color
    };
    for (unsigned i = 0; i < sizeof uniforms / sizeof uniforms[0]; ++i)
        uniforms[i]->idx = UINT16_MAX;
    jce_trace_set_enabled(false);
    jce_render_readback_backend_open(&renderer_token);
}
void tearDown(void)
{
    /* Failed negative assertions still clean up through the real stop barrier. */
    if(borrowed && Unity.CurrentTestFailed)complete_at(ready_frame);
    for(JceRenderReadbackQueue *q=s_readback_queues;q;q=q->next)q->destroy_requested=true;
    jce_render_readback_before_backend_shutdown(&renderer_token);
    backend_joined=true;
    jce_render_readback_after_backend_shutdown(&renderer_token);
    TEST_ASSERT_FALSE(borrowed);TEST_ASSERT_NULL(s_readback_queues);
    TEST_ASSERT_FALSE_MESSAGE(premature_cpu_release,"CPU destination freed while bgfx still borrows it");
    TEST_ASSERT_EQUAL_UINT32(0,s_readback_attached);TEST_ASSERT_EQUAL_UINT32(0,s_readback_retiring);
    TEST_ASSERT_EQUAL_UINT32(0,s_readback_cpu_buffers);TEST_ASSERT_EQUAL_UINT64(0,s_readback_cpu_bytes);
    jce_trace_set_enabled(false);
}

static JceRenderReadbackQueue *submitted_queue(JceRenderReadbackTicket *ticket)
{
    JceRenderReadbackQueue *queue = jce_render_readback_create(&renderer_token);
    JceRenderReadbackDesc desc = jce_render_readback_desc_default();
    TEST_ASSERT_NOT_NULL(queue);
    desc.source.idx = 7;
    desc.format = JCE_RENDER_FORMAT_RGBA8;
    desc.width = desc.height = 2;
    TEST_ASSERT_TRUE(jce_render_readback_submit(queue, &desc, ticket));
    TEST_ASSERT_EQUAL_UINT32(1, read_calls);
    return queue;
}


static void check_production_destroy_with_borrow(bool cancelled)
{
    JceRenderReadbackTicket ticket;
    JceRenderReadbackQueue *queue = submitted_queue(&ticket);
    destroy_expected_queue = queue;
    if (cancelled)
        TEST_ASSERT_TRUE(jce_render_readback_cancel(queue, ticket));

    jce_renderer_destroy_impl(&renderer_token);
    /* The real after-stop hook detached this still caller-owned wrapper.
     * Dispose it before assertions, including for the ordinary mutant. */
    unsigned destroys_before_wrapper_release = texture_destroys;
    jce_render_readback_destroy(queue);

    TEST_ASSERT_FALSE_MESSAGE(premature_cpu_release,
        "CPU destination freed while bgfx still borrows it");
    TEST_ASSERT_TRUE_MESSAGE(destroy_destination_held_at_join,
        "Pending or cancelled destination must survive until backend return");
    TEST_ASSERT_TRUE(destroy_service_closed_at_join);
    TEST_ASSERT_TRUE(destroy_queue_detached_at_join);
    TEST_ASSERT_EQUAL_UINT32(1, destroy_before_calls);
    TEST_ASSERT_EQUAL_UINT32(1, destroy_join_calls);
    TEST_ASSERT_EQUAL_UINT32(1, destroy_after_calls);
    TEST_ASSERT_TRUE(destroy_before_at < destroy_join_at);
    TEST_ASSERT_TRUE(destroy_join_at < destroy_after_at);
    TEST_ASSERT_EQUAL_UINT32(1, pixel_frees);
    TEST_ASSERT_EQUAL_UINT32(1, texture_destroys);
    TEST_ASSERT_EQUAL_UINT32(destroys_before_wrapper_release, texture_destroys);
    TEST_ASSERT_EQUAL_UINT32(1, renderer_frees);
    TEST_ASSERT_EQUAL_UINT32(1, destroy_unbind_calls);
    TEST_ASSERT_EQUAL_PTR(&s_device.payload, destroy_seen_bridge);
    TEST_ASSERT_EQUAL_PTR(&renderer_token.submitted_payload, destroy_seen_payload);
    TEST_ASSERT_EQUAL_UINT32(1, destroy_offscreen_calls);
    TEST_ASSERT_EQUAL_PTR(&renderer_token, destroy_seen_offscreen);
    TEST_ASSERT_EQUAL_UINT32(1, destroy_upload_calls);
    TEST_ASSERT_EQUAL_PTR(&renderer_token.instance_upload, destroy_seen_upload);
    TEST_ASSERT_NULL(s_readback_renderer);
    TEST_ASSERT_NULL(s_readback_queues);
    TEST_ASSERT_EQUAL_UINT32(0, s_readback_attached);
    TEST_ASSERT_EQUAL_UINT32(0, s_readback_retiring);
    TEST_ASSERT_EQUAL_UINT32(0, s_readback_cpu_buffers);
    TEST_ASSERT_EQUAL_UINT64(0, s_readback_cpu_bytes);
    TEST_ASSERT_FALSE(borrowed);
}

static void test_production_destroy_keeps_pending_destination_until_backend_join(void)
{
    check_production_destroy_with_borrow(false);
}

static void test_production_destroy_keeps_cancelled_destination_until_backend_join(void)
{
    check_production_destroy_with_borrow(true);
}

static void test_production_destroy_null_does_not_touch_backend(void)
{
    jce_renderer_destroy_impl(NULL);
    TEST_ASSERT_EQUAL_UINT32(0, destroy_unbind_calls);
    TEST_ASSERT_EQUAL_UINT32(0, destroy_before_calls);
    TEST_ASSERT_EQUAL_UINT32(0, destroy_join_calls);
    TEST_ASSERT_EQUAL_UINT32(0, destroy_after_calls);
    TEST_ASSERT_EQUAL_UINT32(0, renderer_frees);
    TEST_ASSERT_EQUAL_PTR(&renderer_token, s_readback_renderer);
}

static void test_cancel_then_repeated_release_retains_destination_until_ready(void)
{
    JceRenderReadbackTicket ticket;
    JceRenderReadbackQueue *queue = submitted_queue(&ticket);
    void *pixels = destination;
    TEST_ASSERT_TRUE(jce_render_readback_cancel(queue, ticket));
    TEST_ASSERT_TRUE(jce_render_readback_release(queue, ticket));
    TEST_ASSERT_TRUE(jce_render_readback_release(queue, ticket));
    TEST_ASSERT_EQUAL_PTR(pixels, destination);
    TEST_ASSERT_EQUAL_UINT32(0, pixel_frees);
    TEST_ASSERT_EQUAL_UINT32(0, texture_destroys);
    TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_CANCELLED, jce_render_readback_poll(queue, ticket));
    complete_at(ready_frame);
    TEST_ASSERT_EQUAL_UINT8(0x5a, ((uint8_t *)pixels)[0]);
    TEST_ASSERT_TRUE(jce_render_readback_release(queue, ticket));
    TEST_ASSERT_EQUAL_UINT32(1, pixel_frees);
    TEST_ASSERT_EQUAL_UINT32(1, texture_destroys);
    TEST_ASSERT_FALSE(jce_render_readback_release(queue, ticket));
    jce_render_readback_destroy(queue);
    TEST_ASSERT_EQUAL_UINT32(1, pixel_frees);
}

static void test_release_pending_twice_then_poll_and_release_disposes_once(void)
{
    JceRenderReadbackTicket ticket;
    JceRenderReadbackQueue *queue = submitted_queue(&ticket);
    TEST_ASSERT_TRUE(jce_render_readback_release(queue, ticket));
    complete_at(ready_frame - 1u);
    TEST_ASSERT_TRUE(jce_render_readback_release(queue, ticket));
    TEST_ASSERT_TRUE(jce_render_readback_cancel(queue, ticket));
    TEST_ASSERT_EQUAL_UINT32(0, all_frees);
    complete_at(ready_frame);
    TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_CANCELLED, jce_render_readback_poll(queue, ticket));
    TEST_ASSERT_EQUAL_UINT32(1, pixel_frees);
    TEST_ASSERT_TRUE(jce_render_readback_release(queue, ticket));
    TEST_ASSERT_EQUAL_UINT32(1, pixel_frees);
    jce_render_readback_destroy(queue);
    TEST_ASSERT_EQUAL_UINT32(1, texture_destroys);
}

static void test_queue_destroy_observes_real_ready_frame_before_free(void)
{
    JceRenderReadbackTicket ticket;
    JceRenderReadbackQueue *queue = submitted_queue(&ticket);
    TEST_ASSERT_TRUE(jce_render_readback_cancel(queue, ticket));
    jce_render_readback_destroy(queue);
    TEST_ASSERT_EQUAL_UINT32(ready_frame, current_frame);
    TEST_ASSERT_EQUAL_UINT32(ready_frame,s_device.frame_index);
    TEST_ASSERT_EQUAL_UINT32(6,s_device.offscreen_submission_index);
    TEST_ASSERT_EQUAL_UINT32(6,retirement_calls);TEST_ASSERT_EQUAL_UINT32(6,instance_calls);
    TEST_ASSERT_EQUAL_UINT32(1, pixel_frees);
    TEST_ASSERT_EQUAL_UINT32(1, texture_destroys);
}

static void test_texture_creation_failure_releases_cpu_without_readback(void)
{
    JceRenderReadbackQueue *queue = jce_render_readback_create(&renderer_token);
    JceRenderReadbackDesc desc = jce_render_readback_desc_default();
    JceRenderReadbackTicket ticket;
    TEST_ASSERT_NOT_NULL(queue);
    desc.source.idx = 7; desc.format = JCE_RENDER_FORMAT_RGBA8;
    desc.width = desc.height = 2;
    fail_texture = true;
    TEST_ASSERT_FALSE(jce_render_readback_submit(queue, &desc, &ticket));
    TEST_ASSERT_EQUAL_UINT32(JCE_RENDER_READBACK_INVALID_TICKET, ticket);
    TEST_ASSERT_EQUAL_UINT32(1, all_frees);
    TEST_ASSERT_EQUAL_UINT32(0, read_calls);
    TEST_ASSERT_EQUAL_UINT32(0, texture_destroys);
    jce_render_readback_destroy(queue);
    TEST_ASSERT_EQUAL_UINT32(2, all_frees);
}

static void test_default_descriptor_is_invalid_until_authored(void)
{
    JceRenderReadbackDesc desc = jce_render_readback_desc_default();

    TEST_ASSERT_EQUAL_UINT32(sizeof(desc), desc.struct_size);
    TEST_ASSERT_EQUAL_UINT32(JCE_RENDER_READBACK_ABI_VERSION, desc.version);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, desc.source.idx);
    TEST_ASSERT_EQUAL_UINT32(JCE_RENDER_FORMAT_RGBA32F, desc.format);
    TEST_ASSERT_EQUAL_UINT32(0, desc.width);
    TEST_ASSERT_EQUAL_UINT32(0, desc.height);
}

static void test_typed_layout_is_exact_and_overflow_checked(void)
{
    uint64_t row_pitch = 0;
    uint64_t byte_size = 0;

    TEST_ASSERT_TRUE(jce_render_readback_layout(
        JCE_RENDER_FORMAT_RGBA32F, 64, 64, &row_pitch, &byte_size));
    TEST_ASSERT_EQUAL_UINT64(64u * 16u, row_pitch);
    TEST_ASSERT_EQUAL_UINT64(64u * 64u * 16u, byte_size);

    TEST_ASSERT_TRUE(jce_render_readback_layout(
        JCE_RENDER_FORMAT_R16F, 7, 5, &row_pitch, &byte_size));
    TEST_ASSERT_EQUAL_UINT64(14, row_pitch);
    TEST_ASSERT_EQUAL_UINT64(70, byte_size);

    TEST_ASSERT_FALSE(jce_render_readback_layout(
        JCE_RENDER_FORMAT_DEPTH24_STENCIL8, 1, 1,
        &row_pitch, &byte_size));
    TEST_ASSERT_FALSE(jce_render_readback_layout(
        JCE_RENDER_FORMAT_RGBA32F, 0, 1, &row_pitch, &byte_size));
    TEST_ASSERT_FALSE(jce_render_readback_layout(
        JCE_RENDER_FORMAT_RGBA32F, UINT32_MAX, UINT32_MAX,
        &row_pitch, &byte_size));
}

static void test_invalid_ticket_operations_are_total(void)
{
    JceRenderReadbackInfo info;
    unsigned char dst[16];

    memset(&info, 0xa5, sizeof(info));
    TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_INVALID,
        jce_render_readback_poll(NULL, JCE_RENDER_READBACK_INVALID_TICKET));
    TEST_ASSERT_FALSE(jce_render_readback_get_info(
        NULL, JCE_RENDER_READBACK_INVALID_TICKET, &info));
    TEST_ASSERT_FALSE(jce_render_readback_copy(
        NULL, JCE_RENDER_READBACK_INVALID_TICKET, dst, sizeof(dst)));
    TEST_ASSERT_FALSE(jce_render_readback_cancel(
        NULL, JCE_RENDER_READBACK_INVALID_TICKET));
    TEST_ASSERT_FALSE(jce_render_readback_release(
        NULL, JCE_RENDER_READBACK_INVALID_TICKET));
    TEST_ASSERT_EQUAL_UINT32(4, JCE_RENDER_READBACK_QUEUE_CAPACITY);
}


static void test_serial_order_has_explicit_half_range_and_wrap_boundaries(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_RENDERER_FRAME_BEFORE,jce_renderer_frame_order(UINT32_MAX,0));
    TEST_ASSERT_EQUAL_INT(JCE_RENDERER_FRAME_EQUAL,jce_renderer_frame_order(0,0));
    TEST_ASSERT_EQUAL_INT(JCE_RENDERER_FRAME_AFTER,jce_renderer_frame_order(0,UINT32_MAX));
    TEST_ASSERT_EQUAL_INT(JCE_RENDERER_FRAME_AMBIGUOUS,jce_renderer_frame_order(0,UINT32_C(0x80000000)));
    TEST_ASSERT_FALSE(jce_renderer_frame_ready(0,UINT32_C(0x80000000)));
    TEST_ASSERT_FALSE(jce_renderer_frame_ready(UINT32_C(0x80000000),0));
    TEST_ASSERT_TRUE(jce_renderer_frame_ready(UINT32_C(0x7fffffff),0));
    TEST_ASSERT_FALSE(jce_renderer_frame_ready(0,UINT32_C(0x7fffffff)));
}
static void test_wrap_before_at_after_retains_destination_until_actual_delivery(void)
{
    current_frame=s_device.frame_index=UINT32_MAX;ready_frame=1;
    JceRenderReadbackTicket ticket;JceRenderReadbackQueue *queue=submitted_queue(&ticket);
    void *pixels=destination;
    TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_PENDING,jce_render_readback_poll(queue,ticket));
    complete_at(0);TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_PENDING,jce_render_readback_poll(queue,ticket));
    TEST_ASSERT_EQUAL_UINT32(0,pixel_frees);TEST_ASSERT_EQUAL_PTR(pixels,destination);
    complete_at(1);TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_READY,jce_render_readback_poll(queue,ticket));
    complete_at(2);TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_READY,jce_render_readback_poll(queue,ticket));
    unsigned char bytes[16];TEST_ASSERT_TRUE(jce_render_readback_copy(queue,ticket,bytes,sizeof bytes));
    TEST_ASSERT_EQUAL_UINT8(0x5a,bytes[0]);jce_render_readback_destroy(queue);
    TEST_ASSERT_EQUAL_UINT32(1,pixel_frees);TEST_ASSERT_EQUAL_UINT32(0,retirement_calls);
}
static void test_wrap_destroy_advances_only_the_central_owner(void)
{
    current_frame=s_device.frame_index=UINT32_MAX;ready_frame=1;
    JceRenderReadbackTicket ticket;JceRenderReadbackQueue *queue=submitted_queue(&ticket);
    jce_render_readback_destroy(queue);
    TEST_ASSERT_EQUAL_UINT32(1,s_device.frame_index);TEST_ASSERT_EQUAL_UINT32(2,s_device.offscreen_submission_index);
    TEST_ASSERT_EQUAL_UINT32(2,retirement_calls);TEST_ASSERT_EQUAL_UINT32(2,instance_calls);
    TEST_ASSERT_EQUAL_UINT32(1,pixel_frees);
}
static void test_half_range_destroy_defers_without_advancing_or_freeing_borrow(void)
{
    ready_frame=UINT32_C(0x80000000);
    JceRenderReadbackTicket ticket;JceRenderReadbackQueue *queue=submitted_queue(&ticket);
    TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_PENDING,jce_render_readback_poll(queue,ticket));
    jce_render_readback_destroy(queue);
    TEST_ASSERT_EQUAL_UINT32(0,s_device.offscreen_submission_index);TEST_ASSERT_EQUAL_UINT32(0,pixel_frees);
    TEST_ASSERT_TRUE(borrowed);TEST_ASSERT_TRUE(queue->destroy_requested);TEST_ASSERT_NULL(queue->renderer);
    jce_render_readback_before_backend_shutdown(&renderer_token);backend_joined=true;
    jce_render_readback_after_backend_shutdown(&renderer_token);
    TEST_ASSERT_EQUAL_UINT32(1,pixel_frees);TEST_ASSERT_NULL(s_readback_queues);
}
static void test_no_progress_and_long_promise_have_bounded_central_advances(void)
{
    no_advance=true;
    JceRenderReadbackTicket ticket;JceRenderReadbackQueue *queue=submitted_queue(&ticket);
    jce_render_readback_destroy(queue);
    TEST_ASSERT_EQUAL_UINT32(1,retirement_calls);TEST_ASSERT_EQUAL_UINT32(1,instance_calls);
    TEST_ASSERT_EQUAL_UINT32(0,pixel_frees);TEST_ASSERT_TRUE(borrowed);
    jce_render_readback_before_backend_shutdown(&renderer_token);backend_joined=true;
    jce_render_readback_after_backend_shutdown(&renderer_token);TEST_ASSERT_EQUAL_UINT32(1,pixel_frees);
    jce_render_readback_backend_open(&renderer_token);backend_joined=false;no_advance=false;ready_frame=64;
    read_calls=0;queue=submitted_queue(&ticket);jce_render_readback_destroy(queue);
    TEST_ASSERT_EQUAL_UINT32(9,retirement_calls);TEST_ASSERT_EQUAL_UINT32(9,instance_calls);
    TEST_ASSERT_EQUAL_UINT32(1,pixel_frees);TEST_ASSERT_TRUE(borrowed);
}
static void test_backend_stop_detaches_live_wrapper_and_blocks_new_admission(void)
{
    JceRenderReadbackTicket ticket;JceRenderReadbackQueue *queue=submitted_queue(&ticket);
    jce_render_readback_before_backend_shutdown(&renderer_token);
    TEST_ASSERT_NULL(queue->renderer);TEST_ASSERT_NULL(jce_render_readback_create(&renderer_token));
    TEST_ASSERT_EQUAL_UINT32(0,pixel_frees);TEST_ASSERT_TRUE(jce_render_readback_release(queue,ticket));
    TEST_ASSERT_EQUAL_UINT32(0,pixel_frees);backend_joined=true;
    jce_render_readback_after_backend_shutdown(&renderer_token);
    TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_FAILED,jce_render_readback_poll(queue,ticket));
    TEST_ASSERT_EQUAL_UINT32(1,pixel_frees);TEST_ASSERT_EQUAL_UINT32(1,texture_destroys);
    jce_render_readback_backend_open(&renderer_token); /* Address reuse cannot reattach old wrapper. */
    unsigned before=texture_destroys;jce_render_readback_destroy(queue);
    TEST_ASSERT_EQUAL_UINT32(before,texture_destroys);TEST_ASSERT_EQUAL_UINT32(0,retirement_calls);
    TEST_ASSERT_NULL(jce_render_readback_create(NULL));
    TEST_ASSERT_NULL(jce_render_readback_create((JceRenderer *)&caps));
}

static void test_transition_trace_is_cpu_ownership_and_not_per_poll_noise(void)
{
    jce_trace_set_enabled(true);jce_trace_reset();
    JceRenderReadbackTicket ticket;JceRenderReadbackQueue *queue=submitted_queue(&ticket);
    JceTraceStats before,after;jce_trace_get_stats(&before);
    for(unsigned i=0;i<4;++i)TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_PENDING,jce_render_readback_poll(queue,ticket));
    jce_trace_get_stats(&after);TEST_ASSERT_EQUAL_UINT64(before.recorded_events,after.recorded_events);
    jce_render_readback_before_backend_shutdown(&renderer_token);backend_joined=true;
    jce_render_readback_after_backend_shutdown(&renderer_token);
    JceTraceCursor cursor;JceTraceEvent events[64];jce_trace_cursor_init(&cursor);
    uint32_t count=jce_trace_read(&cursor,events,64);bool held=false,retiring=false,released=false;
    for(uint32_t i=0;i<count;++i) {
        if(events[i].type!=JCE_TRACE_EVENT_COUNTER)continue;
        if(strcmp(events[i].name,"readback.cpu_owned_destination_bytes")==0 && events[i].value_u64==16)held=true;
        if(strcmp(events[i].name,"readback.cpu_retiring_queues")==0 && events[i].value_u64==1)retiring=true;
        if(strcmp(events[i].name,"readback.backend_stop_cpu_releases")==0 && events[i].value_u64==1)released=true;
    }
    TEST_ASSERT_TRUE(held);TEST_ASSERT_TRUE(retiring);TEST_ASSERT_TRUE(released);
    jce_render_readback_destroy(queue);TEST_ASSERT_EQUAL_UINT32(1,pixel_frees);
}

static void test_actual_format_admission_requires_blit_readback_and_format(void)
{
    TEST_ASSERT_TRUE(jce_render_readback_format_supported(JCE_RENDER_FORMAT_RGBA8));
    caps.supported=BGFX_CAPS_TEXTURE_READ_BACK;
    TEST_ASSERT_FALSE(jce_render_readback_format_supported(JCE_RENDER_FORMAT_RGBA8));
    caps.supported=BGFX_CAPS_TEXTURE_BLIT;
    TEST_ASSERT_FALSE(jce_render_readback_format_supported(JCE_RENDER_FORMAT_RGBA8));
    caps.supported=BGFX_CAPS_TEXTURE_BLIT|BGFX_CAPS_TEXTURE_READ_BACK;
    caps.formats[BGFX_TEXTURE_FORMAT_RGBA8]=0;
    TEST_ASSERT_FALSE(jce_render_readback_format_supported(JCE_RENDER_FORMAT_RGBA8));
    caps.formats[BGFX_TEXTURE_FORMAT_RGBA16F]=BGFX_CAPS_FORMAT_TEXTURE_2D;
    TEST_ASSERT_TRUE(jce_render_readback_format_supported(JCE_RENDER_FORMAT_RGBA16F));
}

typedef struct ReadbackWitness {
    uint64_t id, operation;
    int64_t context;
    uint32_t ticket, stage, promise, frame;
    bool linked, ready, released, stopped, frame_known;
} ReadbackWitness;
static unsigned readback_witnesses(ReadbackWitness out[16],uint32_t *stop_end,uint32_t *release_at)
{
    JceTraceEvent events[512];JceTraceCursor cursor;jce_trace_cursor_init(&cursor);
    const uint32_t count=jce_trace_read(&cursor,events,512);
    TEST_ASSERT_LESS_THAN_UINT32(512,count);
    unsigned used=0;*stop_end=*release_at=0;
    for(uint32_t i=0;i<count;++i) {
        JceTraceEvent *e=&events[i];
        if(e->type==JCE_TRACE_EVENT_TASK_END && strcmp(e->name,"renderer.backend_shutdown_cpu")==0)
            *stop_end=i+1;
        if(e->type!=JCE_TRACE_EVENT_COUNTER || strncmp(e->name,"readback.op.",12)!=0)continue;
        const char *field=e->name+12;
        if(strcmp(field,"operation_lo")==0) {
            TEST_ASSERT_LESS_THAN_UINT32(16,used);
            memset(&out[used],0,sizeof out[used]);out[used].id=e->id;
            out[used++].operation=(uint32_t)e->value_u64;
        }
        TEST_ASSERT_GREATER_THAN_UINT32(0,used);
        ReadbackWitness *w=&out[used-1];TEST_ASSERT_EQUAL_UINT64(w->id,e->id);
        TEST_ASSERT_EQUAL_UINT64(e->id,e->parent_id);
        const uint64_t v=e->value_u64;
        if(strcmp(field,"operation_hi")==0)w->operation|=v<<32;
        else if(strcmp(field,"context_generation")==0)w->context=(int64_t)v;
        else if(strcmp(field,"ticket")==0)w->ticket=(uint32_t)v;
        else if(strcmp(field,"stage")==0){w->stage=(uint32_t)v;if(v==READBACK_TRACE_STOP_RELEASE)*release_at=i+1;}
        else if(strcmp(field,"ready_frame")==0)w->promise=(uint32_t)v;
        else if(strcmp(field,"observed_frame")==0)w->frame=(uint32_t)v;
        else if(strcmp(field,"capture_linked")==0)w->linked=v!=0;
        else if(strcmp(field,"observed_frame_known")==0)w->frame_known=v!=0;
        else if(strcmp(field,"readback_ready_observed")==0)w->ready=v!=0;
        else if(strcmp(field,"destination_released")==0)w->released=v!=0;
        else if(strcmp(field,"backend_stopped")==0)w->stopped=v!=0;
        else if(strcmp(field,"destination_bytes")==0)TEST_ASSERT_EQUAL_UINT64(16,v);
    }
    return used;
}
static void test_trace_links_submit_ready_and_actual_release(void)
{
    jce_trace_set_enabled(true);jce_trace_reset();
    JceRenderReadbackTicket ticket;JceRenderReadbackQueue *q=submitted_queue(&ticket);
    complete_at(ready_frame);
    TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_READY,jce_render_readback_poll(q,ticket));
    TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_READY,jce_render_readback_poll(q,ticket));
    TEST_ASSERT_TRUE(jce_render_readback_release(q,ticket));
    ReadbackWitness w[16];uint32_t stop,release;
    TEST_ASSERT_EQUAL_UINT32(3,readback_witnesses(w,&stop,&release));
    const uint32_t stages[]={READBACK_TRACE_SUBMIT,READBACK_TRACE_READY,READBACK_TRACE_RELEASE};
    for(unsigned i=0;i<3;++i){TEST_ASSERT_EQUAL_UINT32(stages[i],w[i].stage);TEST_ASSERT_TRUE(w[i].linked);
        TEST_ASSERT_EQUAL_UINT64(w[0].operation,w[i].operation);TEST_ASSERT_EQUAL_UINT32(ticket,w[i].ticket);
        TEST_ASSERT_EQUAL_INT64((int64_t)trace_context,w[i].context);TEST_ASSERT_FALSE(w[i].stopped);}
    TEST_ASSERT_FALSE(w[0].ready);TEST_ASSERT_FALSE(w[0].released);
    TEST_ASSERT_TRUE(w[1].ready);TEST_ASSERT_FALSE(w[1].released);TEST_ASSERT_EQUAL_UINT32(ready_frame,w[1].frame);
    TEST_ASSERT_TRUE(w[2].ready);TEST_ASSERT_TRUE(w[2].released);
    TEST_ASSERT_EQUAL_UINT32(0,stop);jce_render_readback_destroy(q);
    const char *trace_output=getenv("JCE_TEST_READBACK_TRACE_OUTPUT");
    if(trace_output && trace_output[0])TEST_ASSERT_TRUE(jce_trace_export_chrome_json(trace_output));
}
static void test_trace_cancel_is_intent_until_readback_ready(void)
{
    jce_trace_set_enabled(true);jce_trace_reset();
    JceRenderReadbackTicket ticket;JceRenderReadbackQueue *q=submitted_queue(&ticket);
    TEST_ASSERT_TRUE(jce_render_readback_cancel(q,ticket));TEST_ASSERT_TRUE(jce_render_readback_release(q,ticket));
    TEST_ASSERT_EQUAL_UINT32(0,pixel_frees);complete_at(ready_frame);
    TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_CANCELLED,jce_render_readback_poll(q,ticket));
    TEST_ASSERT_TRUE(jce_render_readback_release(q,ticket));
    ReadbackWitness w[16];uint32_t stop,release;TEST_ASSERT_EQUAL_UINT32(4,readback_witnesses(w,&stop,&release));
    TEST_ASSERT_EQUAL_UINT32(READBACK_TRACE_CANCEL,w[1].stage);TEST_ASSERT_FALSE(w[1].ready);TEST_ASSERT_FALSE(w[1].released);
    TEST_ASSERT_EQUAL_UINT32(READBACK_TRACE_READY,w[2].stage);TEST_ASSERT_TRUE(w[2].ready);
    TEST_ASSERT_EQUAL_UINT32(READBACK_TRACE_RELEASE,w[3].stage);TEST_ASSERT_TRUE(w[3].released);
    TEST_ASSERT_EQUAL_UINT32(1,pixel_frees);jce_render_readback_destroy(q);
}
static void test_trace_backend_return_release_never_invents_ready(void)
{
    jce_trace_set_enabled(true);jce_trace_reset();
    JceRenderReadbackTicket ticket;JceRenderReadbackQueue *q=submitted_queue(&ticket);
    destroy_expected_queue=q;jce_renderer_destroy_impl(&renderer_token);
    ReadbackWitness w[16];uint32_t stop,release;TEST_ASSERT_EQUAL_UINT32(2,readback_witnesses(w,&stop,&release));
    TEST_ASSERT_EQUAL_UINT32(READBACK_TRACE_STOP_RELEASE,w[1].stage);
    TEST_ASSERT_TRUE(w[1].stopped);TEST_ASSERT_TRUE(w[1].released);TEST_ASSERT_FALSE(w[1].ready);
    TEST_ASSERT_FALSE(w[1].frame_known);TEST_ASSERT_EQUAL_UINT64(w[0].operation,w[1].operation);
    TEST_ASSERT_GREATER_THAN_UINT32(0,stop);TEST_ASSERT_GREATER_THAN_UINT32(stop,release);
    jce_render_readback_destroy(q);
}
static void test_trace_generation_change_refuses_old_operation_join(void)
{
    jce_trace_set_enabled(true);jce_trace_reset();
    JceRenderReadbackTicket ticket;JceRenderReadbackQueue *q=submitted_queue(&ticket);
    jce_trace_set_enabled(false);jce_trace_reset();jce_trace_set_enabled(true);
    complete_at(ready_frame);TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_READY,jce_render_readback_poll(q,ticket));
    TEST_ASSERT_TRUE(jce_render_readback_release(q,ticket));
    ReadbackWitness w[16];uint32_t stop,release;TEST_ASSERT_EQUAL_UINT32(2,readback_witnesses(w,&stop,&release));
    for(unsigned i=0;i<2;++i){TEST_ASSERT_FALSE(w[i].linked);TEST_ASSERT_EQUAL_UINT64(0,w[i].operation);TEST_ASSERT_EQUAL_INT64(-1,w[i].context);}
    jce_render_readback_destroy(q);
}
static void test_trace_zero_wrap_ready_is_recorded_once(void)
{
    jce_trace_set_enabled(true);jce_trace_reset();ready_frame=0;current_frame=UINT32_MAX;s_device.frame_index=current_frame;
    JceRenderReadbackTicket ticket;JceRenderReadbackQueue *q=submitted_queue(&ticket);
    TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_PENDING,jce_render_readback_poll(q,ticket));
    complete_at(0);TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_READY,jce_render_readback_poll(q,ticket));
    jce_render_readback_destroy(q);
    ReadbackWitness w[16];uint32_t stop,release;TEST_ASSERT_EQUAL_UINT32(3,readback_witnesses(w,&stop,&release));
    TEST_ASSERT_EQUAL_UINT32(0,w[1].promise);TEST_ASSERT_EQUAL_UINT32(0,w[1].frame);TEST_ASSERT_TRUE(w[1].ready);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_actual_format_admission_requires_blit_readback_and_format);
    RUN_TEST(test_trace_links_submit_ready_and_actual_release);
    RUN_TEST(test_trace_cancel_is_intent_until_readback_ready);
    RUN_TEST(test_trace_backend_return_release_never_invents_ready);
    RUN_TEST(test_trace_generation_change_refuses_old_operation_join);
    RUN_TEST(test_trace_zero_wrap_ready_is_recorded_once);
    RUN_TEST(test_production_destroy_keeps_pending_destination_until_backend_join);
    RUN_TEST(test_production_destroy_keeps_cancelled_destination_until_backend_join);
    RUN_TEST(test_production_destroy_null_does_not_touch_backend);
    RUN_TEST(test_transition_trace_is_cpu_ownership_and_not_per_poll_noise);
    RUN_TEST(test_serial_order_has_explicit_half_range_and_wrap_boundaries);
    RUN_TEST(test_wrap_before_at_after_retains_destination_until_actual_delivery);
    RUN_TEST(test_wrap_destroy_advances_only_the_central_owner);
    RUN_TEST(test_half_range_destroy_defers_without_advancing_or_freeing_borrow);
    RUN_TEST(test_no_progress_and_long_promise_have_bounded_central_advances);
    RUN_TEST(test_backend_stop_detaches_live_wrapper_and_blocks_new_admission);
    RUN_TEST(test_default_descriptor_is_invalid_until_authored);
    RUN_TEST(test_typed_layout_is_exact_and_overflow_checked);
    RUN_TEST(test_invalid_ticket_operations_are_total);
    RUN_TEST(test_cancel_then_repeated_release_retains_destination_until_ready);
    RUN_TEST(test_release_pending_twice_then_poll_and_release_disposes_once);
    RUN_TEST(test_queue_destroy_observes_real_ready_frame_before_free);
    RUN_TEST(test_texture_creation_failure_releases_cpu_without_readback);
    return UNITY_END();
}
