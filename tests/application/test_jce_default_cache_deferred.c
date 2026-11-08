/* Actual stock-entry transition helpers and complete draw-admission prefix.
 * Counted renderer retirement, actual lifecycle/policy/property owners. */
#include "unity.h"
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/application/jce_runtime_host_policy.inc.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_lowlevel.h>
#include <jce/renderer/jce_views.h>
#include <jce/os/core/jce_log.h>
#include <stdlib.h>
#include <string.h>
#include JCE_TEST_BACKGROUND_PROPERTY_OWNER
static int fps;
int JCE_CALL jce_engine_get_target_fps(void) { return fps; }
void JCE_CALL jce_engine_set_target_fps(int value) { fps=value; }
void JCE_CALL jce_runtime_pause_audio(JceRuntime *r) { (void)r; }
void JCE_CALL jce_runtime_resume_audio(JceRuntime *r) { (void)r; }
static unsigned resets;static int token;
static JceSceneRenderer *s_scene_renderer;
static JceScene *s_scene;static JceCamera *s_camera;
static bool s_cache_was_transitioning,s_cache_reset_pending,s_kpi_game_shots_active;
static struct { JceRuntimeHostPolicy policy; } s_default_host;
void jce_scene_renderer_reset_entity_caches(JceSceneRenderer *renderer)
{ TEST_ASSERT_EQUAL_PTR(s_scene_renderer,renderer);++resets; }
/* Public-type CPU ports; the generated owner retains the complete scene-draw
 * branch body. GPU producers are inert, not substitute render algorithms. */
static struct {
    bool enabled, bridge, prepare, extent, output, create, fog;
    unsigned enabled_checks, reports, extent_reads, window_reads, renders, presents;
    unsigned render_returns, tail_visits;
    uint32_t window_w, window_h, requested_w, requested_h, allocated_w, allocated_h;
    uint32_t last_render_w, last_render_h;
    uint16_t last_depth;
    int last_fog_w, last_fog_h;
    JceFrameViewportObservation reported;
} observation;
static JceOffscreenTarget *s_post_target;
static float s_last_dt;
static bool s_camera_keep_color, s_camera_keep_depth;
static bool s_no_postfx_for_fixture;
bool JCE_CALL jce_engine_frame_observation_enabled(void)
{ ++observation.enabled_checks; return observation.enabled; }
bool JCE_CALL jce_engine_report_frame_observations(
    const JceFrameViewportObservation *values, uint32_t count, size_t stride)
{
    TEST_ASSERT_EQUAL_UINT(1,count);TEST_ASSERT_EQUAL_UINT(sizeof(*values),stride);
    TEST_ASSERT_NOT_NULL(values);TEST_ASSERT_TRUE(observation.enabled);
    if (values->cpu_outcome==JCE_FRAME_VIEWPORT_CPU_COMPLETED) {
        TEST_ASSERT_GREATER_THAN_UINT(0,observation.render_returns);
        TEST_ASSERT_GREATER_THAN_UINT(0,observation.tail_visits);
    }
    ++observation.reports;observation.reported=*values;return true;
}
bool jce_offscreen_target_get_extent(const JceOffscreenTarget *target,
                                     uint32_t *width, uint32_t *height)
{
    TEST_ASSERT_EQUAL_PTR(s_post_target,target);++observation.extent_reads;
    *width=observation.extent?observation.allocated_w:0;
    *height=observation.extent?observation.allocated_h:0;return observation.extent;
}
#include <jce/application/jce_default_frame_observation.inc.h>
static void observed_window_size(JceWindow *window,uint32_t *width,uint32_t *height)
{
    TEST_ASSERT_NOT_NULL(window);++observation.window_reads;
    *width=observation.window_w;*height=observation.window_h;
}
static void observed_scene_extent(uint32_t width,uint32_t height,uint32_t *rw,uint32_t *rh)
{
    TEST_ASSERT_EQUAL_UINT(observation.window_w,width);TEST_ASSERT_EQUAL_UINT(observation.window_h,height);
    *rw=observation.requested_w;*rh=observation.requested_h;
}
static uint16_t observed_scene_render(JceSceneRenderer *renderer,JceScene *scene,
    const JceCamera *camera,uint16_t base,float dt,const JceSceneRenderConfig *config)
{
    TEST_ASSERT_EQUAL_PTR(s_scene_renderer,renderer);TEST_ASSERT_EQUAL_PTR(s_scene,scene);
    TEST_ASSERT_EQUAL_PTR(s_camera,camera);(void)dt;
    ++observation.renders;observation.last_render_w=config->viewport_width;
    observation.last_render_h=config->viewport_height;
    observation.last_depth=config->scene_depth_tex_handle;
    observation.last_fog_w=config->fog_rt_width;
    observation.last_fog_h=config->fog_rt_height;
    ++observation.render_returns;return base; /* Also the real refusal return. */
}
static JceSceneRenderConfig s_default_scene_cfg(void)
{
    JceSceneRenderConfig config={0};
    config.scene_depth_tex_handle=UINT16_MAX;
    return config;
}
#define s_default_main_taa_requested() false
#define jce_window_get_size observed_window_size
#define jce_scene_renderer_get_postfx(...) ((JcePostFXPipeline*)&token)
#define jce_postfx_is_enabled(...) observation.bridge
#define jce_scene_renderer_has_fullscreen_effect(...) false
#define jce_offscreen_target_create(...) (observation.create?(JceOffscreenTarget*)&token:NULL)
#define jce_postfx_set_view_base(...) ((void)0)
#define jce_render_pipeline_scene_extent observed_scene_extent
#define jce_camera_view(camera) ((void)(camera),jce_m4_identity())
#define jce_camera_proj(camera,aspect,depth) ((void)(camera),(void)(aspect),(void)(depth),jce_m4_identity())
#define jce_gfx_caps() ((JceGfxCaps){false,false})
#define jce_scene_renderer_taa_begin_frame(renderer,w,h,view,proj,color) \
    ((void)(renderer),(void)(w),(void)(h),(void)(view),(void)(proj),(void)(color),false)
#define jce_scene_renderer_set_taa_velocity_enabled(renderer,enabled) ((void)(renderer),(void)(enabled))
#define jce_offscreen_target_prepare_keep(target,w,h,view,proj,clear,color,depth,name) \
    ((void)(target),(void)(w),(void)(h),(void)(view),(void)(proj),(void)(clear), \
     (void)(color),(void)(depth),(void)(name),observation.prepare)
#define jce_offscreen_target_get_view_id(...) JCE_VIEW_RUNTIME_GAME
#define jce_offscreen_target_get_frame_buffer(...) 7u
#define jce_offscreen_target_get_color_texture(...) 17u
#define jce_offscreen_target_get_depth_texture(...) 18u
#define jce_scene_fog_params_from_scene(...) observation.fog
#define jce_scene_renderer_render_planar_reflection(...) ((void)0)
#define jce_default_main_render_camera_overlays(...) ((void)0)
#define jce_scene_renderer_render observed_scene_render
#define jce_default_main_physics_debug(...) ((void)0)
#define jce_scene_renderer_composite_fog(...) ((void)0)
#define jce_scene_renderer_composite_ssr(...) ((void)0)
#define jce_scene_renderer_composite_planar(renderer,view,dst) ((void)(renderer),(void)(view),(void)(dst))
#define jce_scene_renderer_composite_ssgi(renderer,view,dst) ((void)(renderer),(void)(view),(void)(dst))
#define jce_gfx_texture_valid(handle) ((void)(handle),observation.output)
#define jce_scene_renderer_apply_fullscreen_effects(renderer,scene,camera,color,...) (color)
#define jce_postfx_resize(...) ((void)0)
#define jce_postfx_apply(pipeline,color,depth) ((void)(pipeline),(void)(color),(void)(depth))
#define jce_postfx_get_output(...) ((JceTextureHandle){19u})
#define jce_postfx_present(...) (++observation.presents)
#define jce_scene_renderer_taa_end_frame(renderer,view,proj) ((void)(renderer),(void)(view),(void)(proj))
#define jce_renderer_begin_frame_3d(...) ((void)0)
static const char *frame_log_env, *viewport_policy_env;
static unsigned frame_env_reads, policy_env_reads;
static char *counted_env(const char *name)
{
    if (strcmp(name,"JCE_KPI_FRAME_LOG")==0) { ++frame_env_reads;return (char*)frame_log_env; }
    if (strcmp(name,"JCE_KPI_FRAME_LOG_VIEWPORT")==0) { ++policy_env_reads;return (char*)viewport_policy_env; }
    if (strcmp(name,"JCE_NO_POSTFX")==0) return s_no_postfx_for_fixture?(char*)"1":NULL;
    TEST_FAIL_MESSAGE("Unexpected stock draw environment query");return NULL;
}
#define getenv counted_env
#include JCE_TEST_DEFAULT_CACHE_OWNER
void setUp(void)
{
    jce_lifecycle_shutdown();jce_engine_set_run_in_background(false);
    memset(&s_default_host,0,sizeof s_default_host);
    resets=0;fps=0;s_cache_was_transitioning=s_cache_reset_pending=s_kpi_game_shots_active=false;
    frame_log_env=viewport_policy_env=NULL;frame_env_reads=policy_env_reads=0;
    memset(&observation,0,sizeof observation);
    observation.prepare=observation.extent=observation.output=observation.create=true;
    observation.window_w=1200;observation.window_h=800;
    observation.requested_w=600;observation.requested_h=400;
    observation.allocated_w=512;observation.allocated_h=384;
    s_post_target=NULL;s_last_dt=0.016f;s_camera_keep_color=s_camera_keep_depth=false;
    s_no_postfx_for_fixture=false;
    s_scene_renderer=(JceSceneRenderer*)&token;s_scene=(JceScene*)&token;s_camera=(JceCamera*)&token;
    TEST_ASSERT_TRUE(jce_runtime_host_policy_begin(&s_default_host.policy,NULL,true,true,30,15));
}
void tearDown(void) { jce_runtime_host_policy_end(&s_default_host.policy);jce_lifecycle_shutdown(); }
static void test_cpu_transition_does_not_retire_gpu_until_admitted_draw_once(void)
{
    default_player_cache_transition(true);default_player_cache_transition(false);
    TEST_ASSERT_EQUAL_UINT(0,resets);TEST_ASSERT_TRUE(s_cache_reset_pending);
    JceServices svc={0};svc.renderer=(JceRenderer*)&token;
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);app_draw(&svc,NULL);
    TEST_ASSERT_EQUAL_UINT(0,resets);TEST_ASSERT_TRUE(s_cache_reset_pending);
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);app_draw(&svc,NULL);
    TEST_ASSERT_EQUAL_UINT(1,resets);TEST_ASSERT_FALSE(s_cache_reset_pending);
    app_draw(&svc,NULL);default_player_cache_transition(false);
    TEST_ASSERT_EQUAL_UINT(1,resets);
}
static void test_missing_draw_services_retain_cpu_invalidation_for_later(void)
{
    default_player_cache_transition(true);default_player_cache_transition(false);
    app_draw(NULL,NULL);TEST_ASSERT_EQUAL_UINT(0,resets);TEST_ASSERT_TRUE(s_cache_reset_pending);
    JceServices svc={0};svc.renderer=(JceRenderer*)&token;
    app_draw(&svc,NULL);TEST_ASSERT_EQUAL_UINT(1,resets);TEST_ASSERT_FALSE(s_cache_reset_pending);
}
static void assert_pending_draw_admission(bool expected)
{
    JceServices svc={0};svc.renderer=(JceRenderer*)&token;
    const unsigned before=resets;
    default_player_cache_transition(true);default_player_cache_transition(false);
    app_draw(&svc,NULL);
    TEST_ASSERT_EQUAL_UINT(before+(expected?1u:0u),resets);
    TEST_ASSERT_EQUAL_INT(!expected,s_cache_reset_pending);
}
static void test_natural_frame_log_obeys_actual_focus_and_os_pause(void)
{
    frame_log_env="frames.csv";viewport_policy_env="natural";
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);assert_pending_draw_admission(false);
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);assert_pending_draw_admission(true);
    jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);assert_pending_draw_admission(false);
    jce_lifecycle_emit(JCE_LIFECYCLE_RESUME);assert_pending_draw_admission(true);
    TEST_ASSERT_EQUAL_UINT(4,frame_env_reads);TEST_ASSERT_EQUAL_UINT(4,policy_env_reads);
}
static void test_legacy_and_invalid_selectors_preserve_explicit_frame_capture(void)
{
    const char *modes[]={NULL,"","capture","invalid","Natural","natural ","0","1","normal"};
    frame_log_env="frames.csv";
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
    for (unsigned i=0;i<sizeof modes/sizeof modes[0];++i) {
        viewport_policy_env=modes[i];assert_pending_draw_admission(true);
    }
    jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);
    for (unsigned i=0;i<sizeof modes/sizeof modes[0];++i) {
        viewport_policy_env=modes[i];assert_pending_draw_admission(true);
    }
}
static void test_missing_or_empty_frame_log_never_forces_or_reads_selector(void)
{
    const char *logs[]={NULL,""};const char *modes[]={NULL,"","capture","natural","invalid"};
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
    for (unsigned i=0;i<sizeof logs/sizeof logs[0];++i) {
        frame_log_env=logs[i];
        for (unsigned j=0;j<sizeof modes/sizeof modes[0];++j) {
            viewport_policy_env=modes[j];assert_pending_draw_admission(false);
        }
    }
    TEST_ASSERT_EQUAL_UINT(10,frame_env_reads);TEST_ASSERT_EQUAL_UINT(0,policy_env_reads);
}
static void test_explicit_shots_override_natural_and_absent_frame_log(void)
{
    frame_log_env="frames.csv";viewport_policy_env="natural";s_kpi_game_shots_active=true;
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);assert_pending_draw_admission(true);
    jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);assert_pending_draw_admission(true);
    frame_log_env=NULL;assert_pending_draw_admission(true);
    frame_log_env="";assert_pending_draw_admission(true);
    TEST_ASSERT_EQUAL_UINT(4,frame_env_reads);TEST_ASSERT_EQUAL_UINT(2,policy_env_reads);
}
static void reset_observed_interval(void)
{
    observation.enabled_checks=observation.reports=observation.extent_reads=0;
    observation.window_reads=observation.renders=observation.presents=0;
    observation.render_returns=observation.tail_visits=0;
    memset(&observation.reported,0xA5,sizeof observation.reported);
}
static void observed_draw(void)
{
    JceServices services={0};services.renderer=(JceRenderer*)&token;
    services.window=(JceWindow*)&token;actual_observed_draw(&services,NULL);
}
static void assert_no_display_or_imgui_observation(void)
{
    const JceFrameViewportObservation *v=&observation.reported;
    const uint32_t unknown=JCE_FRAME_VIEWPORT_DISPLAY_SIZE_VALID|JCE_FRAME_VIEWPORT_TEXTURE_VALID|
        JCE_FRAME_VIEWPORT_IMAGE_COUNT_VALID|JCE_FRAME_VIEWPORT_QUEUE_COUNT_VALID|
        JCE_FRAME_VIEWPORT_SUBMIT_COUNTS_VALID;
    TEST_ASSERT_EQUAL_UINT(0,v->flags&unknown);
    TEST_ASSERT_EQUAL_UINT(0,v->display_width|v->display_height|v->texture_index|
        v->image_count|v->queued_commands|v->main_submits|v->recording_submits);
}
static void test_observation_disabled_preserves_pod_and_adds_only_enabled_check(void)
{
    JceFrameViewportObservation value,before;
    memset(&value,0xA5,sizeof value);before=value;
    TEST_ASSERT_FALSE(default_frame_observation_begin(&value));
    TEST_ASSERT_EQUAL_MEMORY(&before,&value,sizeof value);
    TEST_ASSERT_EQUAL_UINT(1,observation.enabled_checks);
    reset_observed_interval();observation.bridge=true;observed_draw();
    TEST_ASSERT_EQUAL_UINT(1,observation.enabled_checks);
    TEST_ASSERT_EQUAL_UINT(0,observation.reports);TEST_ASSERT_EQUAL_UINT(0,observation.extent_reads);
    TEST_ASSERT_EQUAL_UINT(1,observation.window_reads);TEST_ASSERT_EQUAL_UINT(1,observation.renders);
}
static void test_actual_early_returns_publish_distinct_skipped_reasons_once(void)
{
    observation.enabled=true;frame_log_env="frames.csv";viewport_policy_env="natural";
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);observed_draw();
    TEST_ASSERT_EQUAL_UINT(1,observation.reports);
    TEST_ASSERT_EQUAL_UINT(JCE_FRAME_VIEWPORT_CPU_SKIPPED,observation.reported.cpu_outcome);
    TEST_ASSERT_EQUAL_UINT(JCE_DEFAULT_OBSERVATION_HOST_POLICY,observation.reported.refusal_reason);
    TEST_ASSERT_EQUAL_UINT(0,observation.reported.flags);TEST_ASSERT_EQUAL_UINT(0,observation.window_reads);
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
    for (unsigned missing=0;missing<5;++missing) {
        JceServices services={0};services.renderer=(JceRenderer*)&token;services.window=(JceWindow*)&token;
        reset_observed_interval();
        if (missing==1) services.renderer=NULL;
        if (missing==2) s_scene_renderer=NULL;
        if (missing==3) s_scene=NULL;
        if (missing==4) s_camera=NULL;
        actual_observed_draw(missing==0?NULL:&services,NULL);
        TEST_ASSERT_EQUAL_UINT(1,observation.reports);
        TEST_ASSERT_EQUAL_UINT(JCE_FRAME_VIEWPORT_CPU_SKIPPED,observation.reported.cpu_outcome);
        TEST_ASSERT_EQUAL_UINT(JCE_DEFAULT_OBSERVATION_MISSING_OWNERS,observation.reported.refusal_reason);
        TEST_ASSERT_EQUAL_UINT(0,observation.reported.flags);TEST_ASSERT_EQUAL_UINT(0,observation.renders);
        TEST_ASSERT_EQUAL_UINT(0,observation.window_reads);assert_no_display_or_imgui_observation();
        s_scene_renderer=(JceSceneRenderer*)&token;s_scene=(JceScene*)&token;s_camera=(JceCamera*)&token;
    }
}
static void test_actual_bridge_request_allocation_and_cpu_tail_remain_separate(void)
{
    observation.enabled=observation.bridge=true;observed_draw();
    TEST_ASSERT_EQUAL_UINT(1,observation.reports);TEST_ASSERT_EQUAL_UINT(1,observation.extent_reads);
    TEST_ASSERT_EQUAL_UINT(7,observation.reported.flags);
    TEST_ASSERT_EQUAL_UINT(600,observation.reported.requested_width);
    TEST_ASSERT_EQUAL_UINT(400,observation.reported.requested_height);
    TEST_ASSERT_EQUAL_UINT(512,observation.reported.allocated_width);
    TEST_ASSERT_EQUAL_UINT(384,observation.reported.allocated_height);
    TEST_ASSERT_EQUAL_UINT(JCE_FRAME_VIEWPORT_CPU_COMPLETED,observation.reported.cpu_outcome);
    TEST_ASSERT_EQUAL_UINT(0,observation.reported.refusal_reason);
    TEST_ASSERT_EQUAL_UINT(1,observation.renders);TEST_ASSERT_EQUAL_UINT(1,observation.tail_visits);
    assert_no_display_or_imgui_observation();
    reset_observed_interval();observation.extent=false;observed_draw();
    TEST_ASSERT_EQUAL_UINT(5,observation.reported.flags);
    TEST_ASSERT_EQUAL_UINT(0,observation.reported.allocated_width|observation.reported.allocated_height);
    TEST_ASSERT_EQUAL_UINT(JCE_FRAME_VIEWPORT_CPU_COMPLETED,observation.reported.cpu_outcome);
    assert_no_display_or_imgui_observation();
}
static void test_actual_bridge_scene_depth_is_independent_of_fog(void)
{
    observation.bridge=true;
    observed_draw();
    TEST_ASSERT_EQUAL_UINT(1,observation.renders);
    TEST_ASSERT_EQUAL_UINT16(18,observation.last_depth);
    TEST_ASSERT_EQUAL_INT(0,observation.last_fog_w);
    TEST_ASSERT_EQUAL_INT(0,observation.last_fog_h);

    reset_observed_interval();
    observation.fog=true;
    observed_draw();
    TEST_ASSERT_EQUAL_UINT(1,observation.renders);
    TEST_ASSERT_EQUAL_UINT16(18,observation.last_depth);
    TEST_ASSERT_EQUAL_INT(600,observation.last_fog_w);
    TEST_ASSERT_EQUAL_INT(400,observation.last_fog_h);

    reset_observed_interval();
    observation.prepare=false;
    observed_draw();
    TEST_ASSERT_EQUAL_UINT(1,observation.renders);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX,observation.last_depth);
    TEST_ASSERT_EQUAL_INT(0,observation.last_fog_w);
    TEST_ASSERT_EQUAL_INT(0,observation.last_fog_h);
}
static void test_actual_direct_fallback_clears_bridge_and_prior_interval_dimensions(void)
{
    observation.enabled=observation.bridge=true;observed_draw();
    reset_observed_interval();observation.output=false;observed_draw();
    TEST_ASSERT_EQUAL_UINT(2,observation.renders);TEST_ASSERT_EQUAL_UINT(1,observation.reports);
    TEST_ASSERT_EQUAL_UINT(1,observation.reported.flags);
    TEST_ASSERT_EQUAL_UINT(1200,observation.reported.requested_width);
    TEST_ASSERT_EQUAL_UINT(800,observation.reported.requested_height);
    TEST_ASSERT_EQUAL_UINT(0,observation.reported.allocated_width|observation.reported.allocated_height);
    TEST_ASSERT_EQUAL_UINT(1200,observation.last_render_w);
    TEST_ASSERT_EQUAL_UINT(800,observation.last_render_h);
    TEST_ASSERT_EQUAL_UINT(JCE_FRAME_VIEWPORT_CPU_COMPLETED,observation.reported.cpu_outcome);
    assert_no_display_or_imgui_observation();
    reset_observed_interval();observation.bridge=false;observation.window_w=1440;observation.window_h=900;
    observed_draw();TEST_ASSERT_EQUAL_UINT(1,observation.renders);
    TEST_ASSERT_EQUAL_UINT(1440,observation.reported.requested_width);
    TEST_ASSERT_EQUAL_UINT(900,observation.reported.requested_height);
    TEST_ASSERT_EQUAL_UINT(0,observation.extent_reads);TEST_ASSERT_EQUAL_UINT(1,observation.reported.flags);
    assert_no_display_or_imgui_observation();
}
static void test_actual_prepare_refusal_and_empty_extent_preserve_fallback_behavior(void)
{
    observation.enabled=observation.bridge=true;observation.prepare=false;
    s_post_target=(JceOffscreenTarget*)&token;observed_draw();
    TEST_ASSERT_EQUAL_UINT(1,observation.renders);TEST_ASSERT_EQUAL_UINT(0,observation.extent_reads);
    TEST_ASSERT_EQUAL_UINT(1,observation.reported.flags);
    TEST_ASSERT_EQUAL_UINT(1200,observation.reported.requested_width);
    TEST_ASSERT_EQUAL_UINT(JCE_FRAME_VIEWPORT_CPU_COMPLETED,observation.reported.cpu_outcome);
    assert_no_display_or_imgui_observation();
    reset_observed_interval();s_post_target=NULL;observation.create=false;observed_draw();
    TEST_ASSERT_EQUAL_UINT(1,observation.renders);TEST_ASSERT_EQUAL_UINT(0,observation.extent_reads);
    TEST_ASSERT_EQUAL_UINT(1,observation.reported.flags);
    TEST_ASSERT_EQUAL_UINT(JCE_FRAME_VIEWPORT_CPU_COMPLETED,observation.reported.cpu_outcome);
    reset_observed_interval();observation.window_w=observation.window_h=0;observed_draw();
    TEST_ASSERT_EQUAL_UINT(1,observation.renders);TEST_ASSERT_EQUAL_UINT(0,observation.reported.flags);
    TEST_ASSERT_EQUAL_UINT(0,observation.reported.requested_width|observation.reported.requested_height);
    TEST_ASSERT_EQUAL_UINT(JCE_FRAME_VIEWPORT_CPU_COMPLETED,observation.reported.cpu_outcome);
    assert_no_display_or_imgui_observation();
}
int main(void)
{
    UNITY_BEGIN();RUN_TEST(test_cpu_transition_does_not_retire_gpu_until_admitted_draw_once);
    RUN_TEST(test_missing_draw_services_retain_cpu_invalidation_for_later);
    RUN_TEST(test_natural_frame_log_obeys_actual_focus_and_os_pause);
    RUN_TEST(test_legacy_and_invalid_selectors_preserve_explicit_frame_capture);
    RUN_TEST(test_missing_or_empty_frame_log_never_forces_or_reads_selector);
    RUN_TEST(test_explicit_shots_override_natural_and_absent_frame_log);
    RUN_TEST(test_observation_disabled_preserves_pod_and_adds_only_enabled_check);
    RUN_TEST(test_actual_early_returns_publish_distinct_skipped_reasons_once);
    RUN_TEST(test_actual_bridge_request_allocation_and_cpu_tail_remain_separate);
    RUN_TEST(test_actual_bridge_scene_depth_is_independent_of_fog);
    RUN_TEST(test_actual_direct_fallback_clears_bridge_and_prior_interval_dimensions);
    RUN_TEST(test_actual_prepare_refusal_and_empty_extent_preserve_fallback_behavior);
    return UNITY_END();
}
