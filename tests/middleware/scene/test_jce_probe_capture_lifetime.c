/* Complete capture + canonical readback owners, real Scene/Camera/Core, typed
 * backend/offscreen ports. Serial delivery is controlled CPU evidence only. */
#include <jce/middleware/scene/jce_scene_probe_capture.h>
#include <jce/renderer/jce_render_readback.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_perf_phase.h>
#include "os/core/jce_memory.h"
#include "os/core/jce_trace_scope_internal.h"
#include "renderer/jce_render_readback_internal.h"
#include "renderer/jce_renderer_caps_internal.h"
#include "renderer/jce_instance_upload.h"
#include "renderer/jce_offscreen_retirement_internal.h"
#include "renderer/jce_renderer_frame_serial_internal.h"
#include "renderer/jce_renderer_submission_trace_internal.h"
#include <bgfx/c99/bgfx.h>
#include "unity.h"
#include <string.h>

/* Typed fields read by the actual included central submission owner. This
 * CPU fixture leaves submitted payload telemetry unavailable and tracing off. */
struct JceRenderer {
    bool is_fallback,headless;
    JceInstanceUploadOwner instance_upload;
    JceSubmittedPayloadStats submitted_payload;
};
static JceRenderer renderer,other_renderer;
static struct { uint32_t frame_index,offscreen_submission_index; } s_device;
static bgfx_caps_t caps;
static unsigned frame_calls,reads,pixel_frees,texture_creates,texture_destroys,target_destroys,bakes;
static bool backend_joined,premature_free,source_hdr,prepared,fail_target,fail_prepare,fail_texture,wrong_format;
static uint32_t promise_distance;
static bgfx_texture_format_t staging_format;
static unsigned char target_token,sr_token;
static JceScene *scene;
static JceEntity probe;
static struct { void *pixels;size_t bytes;uint32_t ready;bool borrowed;bgfx_texture_format_t format; } borrows[64];
static unsigned char baked[6u*16u*16u*8u];
static bool baked_hdr;
static unsigned linear_renders, ordinary_renders;
static const uint16_t half_pattern[]={0x7e00,0xfc00,0x7c00,0xbc00,0x0000,0x3800,0x3c00,0x4000};
static const bgfx_caps_t *caps_port(void) {return &caps;}
static bool current_context_port(void) {return true;}
static uint32_t current_frame_port(const JceRenderer *r) {(void)r;return s_device.frame_index;}
static void complete_at(uint32_t frame)
{
    s_device.frame_index=frame;
    for(unsigned i=0;i<reads;++i) if(borrows[i].pixels && borrows[i].borrowed &&
        jce_renderer_frame_ready(frame,borrows[i].ready)) {
        if(borrows[i].format==BGFX_TEXTURE_FORMAT_RGBA16F) {
            for(size_t c=0;c<borrows[i].bytes/2;++c)
                memcpy((unsigned char *)borrows[i].pixels+c*2,&half_pattern[c%8],2);
        } else memset(borrows[i].pixels,(int)(i+1),borrows[i].bytes);
        borrows[i].borrowed=false;
    }
}
static uint32_t frame_port(bool capture)
{(void)capture;++frame_calls;complete_at(s_device.frame_index+1u);return s_device.frame_index;}
static bgfx_texture_handle_t create_port(uint16_t w,uint16_t h,bool mips,uint16_t layers,
    bgfx_texture_format_t format,uint64_t flags,const bgfx_memory_t *memory,uint16_t skip)
{
    (void)w;(void)h;(void)mips;(void)layers;(void)flags;(void)memory;(void)skip;
    staging_format=format;++texture_creates;
    return (bgfx_texture_handle_t){fail_texture ? UINT16_MAX : (uint16_t)texture_creates};
}
static uint32_t read_port(bgfx_texture_handle_t texture,void *pixels,uint8_t mip,uint8_t layer)
{
    (void)texture;(void)mip;(void)layer;
    TEST_ASSERT_LESS_THAN_UINT(64,reads);
    borrows[reads].pixels=pixels;
    borrows[reads].format=staging_format;
    borrows[reads].bytes=16u*16u*(staging_format==BGFX_TEXTURE_FORMAT_RGBA16F ? 8u : 4u);
    borrows[reads].ready=s_device.frame_index+promise_distance;
    borrows[reads].borrowed=true;++reads;
    return s_device.frame_index+promise_distance;
}
static void destroy_port(bgfx_texture_handle_t texture) {(void)texture;++texture_destroys;}
static void blit_port(void)
{ if(staging_format!=(source_hdr ? BGFX_TEXTURE_FORMAT_RGBA16F : BGFX_TEXTURE_FORMAT_RGBA8)) wrong_format=true; }
static void free_port(void *data)
{
    if(!data)return;
    for(unsigned i=0;i<reads;++i) if(borrows[i].pixels==data) {
        if(borrows[i].borrowed && !backend_joined) premature_free=true;
        borrows[i].pixels=NULL;borrows[i].borrowed=false;++pixel_frees;
    }
    jce_free(data);
}
static void offscreen_collect_port(const JceRenderer *r,uint32_t ordinal)
{
    TEST_ASSERT_EQUAL_PTR(&renderer,r);
    TEST_ASSERT_EQUAL_UINT32(s_device.offscreen_submission_index,ordinal);
}
static void instance_advance_port(JceInstanceUploadOwner *owner,uint32_t ordinal)
{
    TEST_ASSERT_EQUAL_PTR(&renderer.instance_upload,owner);
    TEST_ASSERT_EQUAL_UINT32(s_device.offscreen_submission_index,ordinal);
}
#undef JCE_FREE
#define JCE_FREE free_port
#define bgfx_get_caps caps_port
#define bgfx_create_texture_2d create_port
#define bgfx_read_texture read_port
#define bgfx_destroy_texture destroy_port
#define bgfx_blit(...) blit_port()
#define bgfx_frame frame_port
#define jce_renderer_get_frame_index current_frame_port
#define jce_offscreen_retirement_collect offscreen_collect_port
#define jce_instance_upload_owner_advance instance_advance_port
#define jce_renderer_submit_frame fixture_submit_frame
#define jce_renderer_frame_admission_current_internal fixture_frame_admission
#define jce_renderer_caps_api_web_context_current current_context_port
#include JCE_TEST_PROBE_SUBMISSION_OWNER
#ifndef JCE_TEST_PROBE_READBACK_OWNER
#define JCE_TEST_PROBE_READBACK_OWNER "renderer/jce_render_readback.c"
#endif
#include JCE_TEST_PROBE_READBACK_OWNER
#undef LOG_TAG

static JceOffscreenTarget *target_create(JceRenderer *r,uint16_t view)
{(void)r;(void)view;prepared=false;return fail_target ? NULL : (JceOffscreenTarget *)&target_token;}
static void target_destroy(JceOffscreenTarget *target) {(void)target;++target_destroys;prepared=false;}
static bool target_prepare(JceOffscreenTarget *target,uint32_t w,uint32_t h,const float *view,
    const float *proj,uint32_t clear,const char *name)
{(void)target;(void)w;(void)h;(void)view;(void)proj;(void)clear;(void)name;prepared=!fail_prepare;return prepared;}
static bool target_hdr(const JceOffscreenTarget *target) {(void)target;return prepared && source_hdr;}
static uint16_t target_handle(const JceOffscreenTarget *target) {(void)target;return 42;}
static JceSceneRenderConfig config_port(void) {JceSceneRenderConfig config={0};return config;}
static uint16_t render_port(JceSceneRenderer *sr,JceScene *s,const JceCamera *camera,
    uint16_t view,float dt,const JceSceneRenderConfig *config)
{(void)sr;(void)s;(void)camera;(void)view;(void)dt;(void)config;++ordinary_renders;return 0;}
static uint16_t render_linear_port(JceSceneRenderer *sr,JceScene *s,const JceCamera *camera,
    uint16_t view,float dt,const JceSceneRenderConfig *config)
{
    (void)sr;(void)s;(void)camera;(void)view;(void)dt;
    TEST_ASSERT_NOT_NULL(config);
    TEST_ASSERT_FALSE(config->apply_postfx);
    TEST_ASSERT_TRUE(config->reduced_pass);
    ++linear_renders;return 0;
}
static JceReflectionProbeBakeHandle bake_port(const JceReflectionProbeBakeDesc *desc,const unsigned char *faces)
{
    ++bakes;baked_hdr=desc->hdr;
    memcpy(baked,faces,6u*16u*16u*(desc->hdr ? 8u : 4u));
    return 1;
}
#define jce_offscreen_target_create target_create
#define jce_offscreen_target_destroy target_destroy
#define jce_offscreen_target_prepare target_prepare
#define jce_offscreen_target_is_hdr target_hdr
#define jce_offscreen_target_get_frame_buffer target_handle
#define jce_offscreen_target_get_color_texture target_handle
#define jce_scene_render_config_default config_port
#define jce_scene_renderer_render render_port
#define jce_scene_renderer_render_linear render_linear_port
#define jce_reflection_probe_bake_submit_faces bake_port
#ifndef JCE_TEST_PROBE_CAPTURE_OWNER
#define JCE_TEST_PROBE_CAPTURE_OWNER "middleware/scene/jce_scene_probe_capture.c"
#endif
#include JCE_TEST_PROBE_CAPTURE_OWNER

void setUp(void)
{
    memset(&s_device,0,sizeof s_device);memset(&renderer,0,sizeof renderer);
    memset(borrows,0,sizeof borrows);memset(baked,0,sizeof baked);memset(&caps,0,sizeof caps);
    caps.supported=BGFX_CAPS_TEXTURE_BLIT|BGFX_CAPS_TEXTURE_READ_BACK;
    caps.formats[BGFX_TEXTURE_FORMAT_RGBA8]=BGFX_CAPS_FORMAT_TEXTURE_2D;
    caps.formats[BGFX_TEXTURE_FORMAT_RGBA16F]=BGFX_CAPS_FORMAT_TEXTURE_2D;
    frame_calls=reads=pixel_frees=texture_creates=texture_destroys=target_destroys=bakes=0;
    backend_joined=premature_free=prepared=fail_target=fail_prepare=fail_texture=wrong_format=false;
    source_hdr=true;promise_distance=2;baked_hdr=false;
    linear_renders=ordinary_renders=0;
    jce_trace_set_enabled(false);jce_perf_phase_set_enabled(false);
    jce_render_readback_backend_open(&renderer);
    scene=jce_scene_create();TEST_ASSERT_NOT_NULL(scene);
    probe=jce_scene_create_entity(scene,"probe");
    JceReflectionProbeComponent component={0};component.hdr=true;
    jce_scene_set_reflection_probe(scene,probe,&component);
}
void tearDown(void)
{
    jce_scene_probe_capture_cancel();
    jce_render_readback_before_backend_shutdown(&renderer);
    backend_joined=true;
    jce_render_readback_after_backend_shutdown(&renderer);
    jce_scene_destroy(scene);
    TEST_ASSERT_NULL(s_readback_queues);
    TEST_ASSERT_EQUAL_UINT(0,s_readback_cpu_buffers);
    TEST_ASSERT_EQUAL_UINT64(0,s_readback_cpu_bytes);
    TEST_ASSERT_FALSE_MESSAGE(premature_free,"probe freed a destination while backend borrowed it");
}
static void begin_capture(bool hdr)
{
    JceReflectionProbeComponent component=*jce_scene_get_reflection_probe(scene,probe);
    component.hdr=hdr;jce_scene_set_reflection_probe(scene,probe,&component);
    TEST_ASSERT_TRUE(jce_scene_probe_capture_begin(scene,probe,16,"probe-test.ktx"));
}
static JceProbeCaptureStatus poll_capture(void)
{return jce_scene_probe_capture_poll((JceSceneRenderer *)&sr_token,scene,&renderer);}
static void render_pending(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_PROBE_CAPTURE_RENDERING,poll_capture());
    TEST_ASSERT_TRUE(jce_scene_probe_capture_in_flight());
}
static void test_pending_cancel_never_submits_or_frees_borrowed_destination(void)
{
    begin_capture(true);render_pending();TEST_ASSERT_EQUAL_UINT(1,reads);
    jce_scene_probe_capture_cancel();jce_scene_probe_capture_cancel();
    TEST_ASSERT_FALSE(jce_scene_probe_capture_in_flight());
    TEST_ASSERT_EQUAL_UINT(0,frame_calls);TEST_ASSERT_EQUAL_UINT(0,pixel_frees);
    TEST_ASSERT_EQUAL_UINT(1,s_readback_retiring);TEST_ASSERT_EQUAL_UINT(1,s_readback_cpu_buffers);
    TEST_ASSERT_EQUAL_INT(JCE_PROBE_CAPTURE_IDLE,poll_capture());
    TEST_ASSERT_EQUAL_UINT(1,reads);TEST_ASSERT_FALSE(premature_free);
    fixture_submit_frame(&renderer);TEST_ASSERT_EQUAL_UINT(0,pixel_frees);
    fixture_submit_frame(&renderer);TEST_ASSERT_EQUAL_UINT(1,pixel_frees);
    TEST_ASSERT_NULL(s_readback_queues);
}
static void test_invalid_or_changed_scene_and_renderer_retire_without_late_copy(void)
{
    begin_capture(true);render_pending();
    TEST_ASSERT_EQUAL_INT(JCE_PROBE_CAPTURE_FAILED,
        jce_scene_probe_capture_poll((JceSceneRenderer *)&sr_token,NULL,&renderer));
    TEST_ASSERT_EQUAL_UINT(0,pixel_frees);TEST_ASSERT_EQUAL_UINT(0,frame_calls);
    TEST_ASSERT_EQUAL_INT(JCE_PROBE_CAPTURE_FAILED,poll_capture());
    JceScene *other=jce_scene_create();TEST_ASSERT_NOT_NULL(other);
    begin_capture(true);render_pending();
    JceProbeCaptureStatus changed=jce_scene_probe_capture_poll((JceSceneRenderer *)&sr_token,other,&renderer);
    jce_scene_destroy(other);TEST_ASSERT_EQUAL_INT(JCE_PROBE_CAPTURE_FAILED,changed);
    begin_capture(true);render_pending();
    TEST_ASSERT_EQUAL_INT(JCE_PROBE_CAPTURE_FAILED,
        jce_scene_probe_capture_poll((JceSceneRenderer *)&sr_token,scene,&other_renderer));
    TEST_ASSERT_EQUAL_UINT(0,bakes);TEST_ASSERT_EQUAL_UINT(0,frame_calls);TEST_ASSERT_EQUAL_UINT(0,pixel_frees);
}
static void test_backend_stop_is_final_release_and_late_poll_is_safe(void)
{
    promise_distance=1000;begin_capture(true);render_pending();
    jce_render_readback_before_backend_shutdown(&renderer);
    TEST_ASSERT_EQUAL_UINT(0,pixel_frees);
    backend_joined=true;jce_render_readback_after_backend_shutdown(&renderer);
    TEST_ASSERT_EQUAL_UINT(1,pixel_frees);
    TEST_ASSERT_EQUAL_INT(JCE_PROBE_CAPTURE_FAILED,poll_capture());
    TEST_ASSERT_NULL(g_cap.readback);TEST_ASSERT_EQUAL_UINT(0,frame_calls);
}
static void test_cancel_restart_is_bounded_until_actual_central_progress(void)
{
    promise_distance=1000;
    for(unsigned i=0;i<JCE_RENDER_READBACK_QUEUE_CAPACITY;++i) {
        begin_capture(true);render_pending();jce_scene_probe_capture_cancel();
    }
    TEST_ASSERT_EQUAL_UINT(JCE_RENDER_READBACK_QUEUE_CAPACITY,reads);
    begin_capture(true);TEST_ASSERT_EQUAL_INT(JCE_PROBE_CAPTURE_FAILED,poll_capture());
    TEST_ASSERT_EQUAL_UINT(JCE_RENDER_READBACK_QUEUE_CAPACITY,reads);
    TEST_ASSERT_EQUAL_UINT(0,frame_calls);TEST_ASSERT_EQUAL_UINT(0,pixel_frees);
    complete_at(1000);fixture_submit_frame(&renderer);
    TEST_ASSERT_NULL(s_readback_queues);TEST_ASSERT_EQUAL_UINT(reads,pixel_frees);
    begin_capture(true);render_pending();TEST_ASSERT_EQUAL_UINT(JCE_RENDER_READBACK_QUEUE_CAPACITY+1,reads);
}
static void test_existing_capture_queue_survives_unrelated_retirement_pressure(void)
{
    begin_capture(true);render_pending();promise_distance=1000;
    for(unsigned i=0;i<JCE_RENDER_READBACK_QUEUE_CAPACITY;++i) {
        JceRenderReadbackQueue *queue=jce_render_readback_create(&renderer);
        TEST_ASSERT_NOT_NULL(queue);
        JceRenderReadbackDesc desc=jce_render_readback_desc_default();
        desc.source=(JceTextureHandle){42};desc.format=JCE_RENDER_FORMAT_RGBA16F;
        desc.width=desc.height=16;
        JceRenderReadbackTicket ticket;
        TEST_ASSERT_TRUE(jce_render_readback_submit(queue,&desc,&ticket));
        jce_render_readback_destroy_deferred(queue);
    }
    fixture_submit_frame(&renderer);fixture_submit_frame(&renderer);
    render_pending(); /* Consume the completed first face. */
    TEST_ASSERT_EQUAL_UINT(4,s_readback_retiring);
    render_pending(); /* Existing queue may render its next face without new admission. */
    TEST_ASSERT_EQUAL_UINT(6,reads);TEST_ASSERT_TRUE(jce_scene_probe_capture_in_flight());
}
static void finish_capture(void)
{
    JceProbeCaptureStatus status=JCE_PROBE_CAPTURE_RENDERING;
    for(unsigned i=0;i<32 && status==JCE_PROBE_CAPTURE_RENDERING;++i) {
        status=poll_capture();
        if(status==JCE_PROBE_CAPTURE_RENDERING) fixture_submit_frame(&renderer);
    }
    TEST_ASSERT_EQUAL_INT(JCE_PROBE_CAPTURE_DONE,status);
    TEST_ASSERT_EQUAL_UINT(6,reads);TEST_ASSERT_EQUAL_UINT(6,pixel_frees);
    TEST_ASSERT_EQUAL_UINT(1,bakes);TEST_ASSERT_FALSE(wrong_format);
    TEST_ASSERT_EQUAL_UINT(6,linear_renders);TEST_ASSERT_EQUAL_UINT(0,ordinary_renders);
    TEST_ASSERT_NULL(s_readback_queues);TEST_ASSERT_FALSE(premature_free);
}
static void test_prepared_hdr_source_preserves_authored_hdr_payload(void)
{
    begin_capture(true);finish_capture();TEST_ASSERT_TRUE(baked_hdr);
    for(unsigned face=0;face<6;++face)
        TEST_ASSERT_EQUAL_MEMORY(half_pattern,baked+face*16u*16u*8u,sizeof half_pattern);
}
static void test_hdr_source_to_ldr_clamps_nonfinite_values_and_rounds_in_place(void)
{
    static const unsigned char expected[]={0,0,255,0,0,128,255,255};
    begin_capture(false);finish_capture();TEST_ASSERT_FALSE(baked_hdr);
    for(unsigned face=0;face<6;++face)
        TEST_ASSERT_EQUAL_MEMORY(expected,baked+face*16u*16u*4u,sizeof expected);
}
static void test_rgba8_fallback_has_exact_six_face_output_and_slot_reuse(void)
{
    source_hdr=false;begin_capture(true);render_pending();
    JceRenderReadbackTicket first=g_cap.ticket;
    fixture_submit_frame(&renderer);fixture_submit_frame(&renderer);render_pending();
    TEST_ASSERT_EQUAL_INT(JCE_RENDER_READBACK_INVALID,jce_render_readback_poll(g_cap.readback,first));
    finish_capture();TEST_ASSERT_FALSE(baked_hdr);
    for(unsigned face=0;face<6;++face) TEST_ASSERT_EQUAL_UINT8(face+1,baked[face*16u*16u*4u]);
}
static void test_target_prepare_and_staging_failures_leave_no_cpu_destination(void)
{
    fail_target=true;begin_capture(true);TEST_ASSERT_EQUAL_INT(JCE_PROBE_CAPTURE_FAILED,poll_capture());
    fail_target=false;fail_prepare=true;begin_capture(true);TEST_ASSERT_EQUAL_INT(JCE_PROBE_CAPTURE_FAILED,poll_capture());
    fail_prepare=false;fail_texture=true;begin_capture(true);TEST_ASSERT_EQUAL_INT(JCE_PROBE_CAPTURE_FAILED,poll_capture());
    TEST_ASSERT_EQUAL_UINT(0,reads);TEST_ASSERT_EQUAL_UINT(0,s_readback_cpu_buffers);
    TEST_ASSERT_NULL(s_readback_queues);TEST_ASSERT_EQUAL_UINT(0,frame_calls);
    TEST_ASSERT_EQUAL_UINT(1,linear_renders);TEST_ASSERT_EQUAL_UINT(0,ordinary_renders);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pending_cancel_never_submits_or_frees_borrowed_destination);
    RUN_TEST(test_invalid_or_changed_scene_and_renderer_retire_without_late_copy);
    RUN_TEST(test_backend_stop_is_final_release_and_late_poll_is_safe);
    RUN_TEST(test_cancel_restart_is_bounded_until_actual_central_progress);
    RUN_TEST(test_existing_capture_queue_survives_unrelated_retirement_pressure);
    RUN_TEST(test_prepared_hdr_source_preserves_authored_hdr_payload);
    RUN_TEST(test_hdr_source_to_ldr_clamps_nonfinite_values_and_rounds_in_place);
    RUN_TEST(test_rgba8_fallback_has_exact_six_face_output_and_slot_reuse);
    RUN_TEST(test_target_prepare_and_staging_failures_leave_no_cpu_destination);
    return UNITY_END();
}
