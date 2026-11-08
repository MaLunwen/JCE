/* Actual bootstrap control blocks; typed CPU ports never create a host/GPU. */
#include "unity.h"
#include "application/jce_engine_launch_policy.h"
#include "application/jce_engine_window_policy.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "application/jce_engine_frame_cap.inc.h"
#include "renderer/jce_renderer_lifecycle_internal.h"
#include <jce/os/core/jce_config.h>
#include <string.h>

/* Private declaration port for the extracted Engine state writes. This is
 * not an Engine ABI/layout substitute or a copied lifecycle implementation. */
typedef enum JceEngineOwnerState {
    JCE_ENGINE_CONSTRUCTING,
    JCE_ENGINE_READY,
    JCE_ENGINE_DRAINING
} JceEngineOwnerState;

struct JceEngine {
    bool headless, hidden_window;
    uint32_t max_frames, frame_index;
    double max_seconds;
    uint64_t frame_counter_prev, run_start_counter;
    JceWindow *window;
    JceRenderer *renderer;
    void *input;
    JceConfig config;
    JceEngineLaunchPolicy launch_policy;
    bool launch_policy_bound;
    JceEngineOwnerState owner_state;
    bool constructor_active;
};
static JceEngine boot;
static int normal_token, fallback_token, headless_token, input_token;
static struct {
    unsigned normal, fallback, headless, bind, warning, clock, failed, joined;
    bool fail_normal, fail_fallback, fail_headless, not_focusable;
    unsigned warning_logs, env_reads;
    bool present, value, requested, available;
    JceRendererBackend backend;
    JceRendererConfig config;
} calls;

static JceRenderer *port_create(JceWindow *window, const JceRendererConfig *config)
{
    (void)window; ++calls.normal; calls.config = *config;
    return calls.fail_normal ? NULL : (JceRenderer *)&normal_token;
}
static JceRenderer *port_create_fallback(JceWindow *window)
{
    (void)window; ++calls.fallback;
    return calls.fail_fallback ? NULL : (JceRenderer *)&fallback_token;
}
static JceRenderer *port_create_headless(void)
{
    ++calls.headless;
    return calls.fail_headless ? NULL : (JceRenderer *)&headless_token;
}
static bool port_is_fallback(const JceRenderer *renderer)
{
    return renderer == (const JceRenderer *)&fallback_token;
}
static JceRendererBackend port_active_backend(void) { return calls.backend; }
static void port_vsync_bind(JceRenderer *renderer, bool present, bool value,
                            bool requested, bool available)
{
    (void)renderer; ++calls.bind; calls.present = present; calls.value = value;
    calls.requested = requested; calls.available = available;
}
static void port_clock(JceEngine *engine) { engine->frame_counter_prev=1000; ++calls.clock; }
static const char *budget_frames, *budget_seconds;
static const char *port_getenv(const char *key)
{
    ++calls.env_reads;
    if(strcmp(key,"JCE_MAX_FRAMES")==0)return budget_frames;
    TEST_ASSERT_EQUAL_STRING("JCE_MAX_SECONDS",key);return budget_seconds;
}
static uint64_t port_counter(void) { return 2000; }
static const char *warning_payload="Hardware acceleration could not be initialized.\nThe application will now run in safe fallback mode.";
static void port_log(int level,const char *tag,const char *file,int line,const char *fmt,...)
{
    (void)tag;(void)file;(void)line;char text[512];va_list args;
    va_start(args,fmt);vsnprintf(text,sizeof text,fmt,args);va_end(args);
    if(strcmp(text,warning_payload)==0){TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_WARN,level);++calls.warning_logs;}
}
static void *port_window(JceWindow *window) { return window; }
static int port_warning(unsigned flags, const char *title, const char *message,
                        void *window)
{
    (void)window; TEST_ASSERT_EQUAL_UINT(1u,flags);
    TEST_ASSERT_EQUAL_STRING("JCE - GPU Unsupported",title);
    TEST_ASSERT_EQUAL_STRING(warning_payload,message);++calls.warning; return 0;
}
static void port_fatal(bool allow,const char *format, ...) { (void)allow;(void)format; }

/* Real declarations above remain untouched. These ports replace only native
 * creation, warning, clock and Renderer binding; the FPS owner is complete. */
#define jce_renderer_create port_create
#define jce_renderer_create_fallback port_create_fallback
#define jce_renderer_create_headless port_create_headless
#define jce_renderer_is_fallback port_is_fallback
#define jce_renderer_get_active_backend port_active_backend
#define jce_renderer_launch_vsync_bind_internal port_vsync_bind
#define jce_engine_reset_frame_clock port_clock
#define jce_window_sdl port_window
#define SDL_ShowSimpleMessageBox port_warning
#define SDL_MESSAGEBOX_WARNING 1u
#define fatal_msg port_fatal
#define LOG_TAG "launch-bootstrap-control"
#define JCE_ENGINE_TRACE_RESULT(name_, call_, result_) \
    do { (void)(name_); call_; (void)(result_); } while (0)

#define getenv port_getenv
#define SDL_atoi atoi
#define jce_time_perf_counter port_counter
#define jce_log_write port_log
#include JCE_TEST_LAUNCH_BOOTSTRAP_HELPERS
static JceAppResult actual_bootstrap(JceEngine *e)
{
    const bool not_focusable=calls.not_focusable;
#include JCE_TEST_LAUNCH_BOOTSTRAP_OWNER
    ++calls.joined;
    return JCE_APP_CONTINUE;
fail:
    ++calls.failed;
    return JCE_APP_FAILURE;
}

void setUp(void)
{
    memset(&s_frame_cap, 0, sizeof s_frame_cap);
    memset(&boot, 0, sizeof boot); memset(&calls, 0, sizeof calls);
    calls.backend = JCE_BACKEND_D3D11;
    budget_frames=budget_seconds=NULL;
    boot.config.vsync = true; boot.config.clear_color = 0x123456ff;
    boot.input = &input_token;
    boot.launch_policy.vsync_present = true;
    boot.launch_policy.fps_present = true;
    boot.owner_state = JCE_ENGINE_CONSTRUCTING;
    boot.constructor_active = true;
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0); jce_trace_reset();
    jce_engine_set_target_fps(144);
}
void tearDown(void)
{
    jce_engine_launch_fps_end(&boot);
    jce_engine_launch_fps_end(&normal_token);
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0);
}

static void assert_bound(bool available)
{
    TEST_ASSERT_TRUE(boot.launch_policy_bound);
    TEST_ASSERT_EQUAL_PTR(&boot, s_frame_cap.launch_owner);
    TEST_ASSERT_EQUAL_UINT(1, calls.bind);
    TEST_ASSERT_TRUE(calls.present); TEST_ASSERT_FALSE(calls.value);
    TEST_ASSERT_TRUE(calls.requested); TEST_ASSERT_EQUAL_INT(available, calls.available);
    TEST_ASSERT_EQUAL_INT(0, jce_engine_get_target_fps());
    jce_engine_set_target_fps(60);
    TEST_ASSERT_EQUAL_INT(0, jce_engine_get_target_fps());
    jce_engine_launch_fps_end(&boot);
    TEST_ASSERT_EQUAL_INT(60, jce_engine_get_target_fps());
}
static void test_normal_binds_once_and_resolves_initial_config(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, actual_bootstrap(&boot)); assert_bound(true);
    TEST_ASSERT_EQUAL_UINT(1, calls.normal); TEST_ASSERT_EQUAL_UINT(0, calls.fallback);
    TEST_ASSERT_EQUAL_UINT(0, calls.headless); TEST_ASSERT_EQUAL_UINT(1, calls.joined);
    TEST_ASSERT_EQUAL_UINT(0, calls.warning); TEST_ASSERT_EQUAL_UINT(0, calls.clock);
    TEST_ASSERT_FALSE(calls.config.vsync);
    TEST_ASSERT_EQUAL_HEX32(boot.config.clear_color, calls.config.clear_color);
    TEST_ASSERT_EQUAL_PTR(&input_token, boot.input);
    /* This extraction stops before the ordinary constructor completion. */
    TEST_ASSERT_TRUE(boot.constructor_active);
    TEST_ASSERT_EQUAL_INT(JCE_ENGINE_CONSTRUCTING, boot.owner_state);
}
static void test_headless_binds_once_then_preserves_original_join(void)
{
    boot.headless = true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, actual_bootstrap(&boot)); assert_bound(false);
    TEST_ASSERT_EQUAL_UINT(1, calls.headless); TEST_ASSERT_EQUAL_UINT(0, calls.normal);
    TEST_ASSERT_EQUAL_UINT(0, calls.fallback); TEST_ASSERT_EQUAL_UINT(1, calls.joined);
    TEST_ASSERT_EQUAL_UINT(0, calls.warning); TEST_ASSERT_NULL(boot.input);
    TEST_ASSERT_TRUE(boot.constructor_active);
    TEST_ASSERT_EQUAL_INT(JCE_ENGINE_CONSTRUCTING, boot.owner_state);
}
static void test_fallback_binds_before_its_existing_early_return(void)
{
    calls.fail_normal = true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, actual_bootstrap(&boot)); assert_bound(false);
    TEST_ASSERT_EQUAL_UINT(1, calls.normal); TEST_ASSERT_EQUAL_UINT(1, calls.fallback);
    TEST_ASSERT_EQUAL_UINT(0, calls.headless); TEST_ASSERT_EQUAL_UINT(0, calls.joined);
    TEST_ASSERT_EQUAL_UINT(1, calls.warning); TEST_ASSERT_EQUAL_UINT(1, calls.clock);
    TEST_ASSERT_FALSE(boot.constructor_active);
    TEST_ASSERT_EQUAL_INT(JCE_ENGINE_READY, boot.owner_state);
}
static void test_creation_failures_never_bind_or_claim_successful_join(void)
{
    calls.fail_normal = true; calls.fail_fallback = true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, actual_bootstrap(&boot));
    TEST_ASSERT_FALSE(boot.launch_policy_bound); TEST_ASSERT_NULL(s_frame_cap.launch_owner);
    TEST_ASSERT_EQUAL_UINT(0, calls.bind); TEST_ASSERT_EQUAL_UINT(0, calls.joined);
    TEST_ASSERT_EQUAL_UINT(1, calls.failed); TEST_ASSERT_EQUAL_INT(144, jce_engine_get_target_fps());
    TEST_ASSERT_TRUE(boot.constructor_active);
    TEST_ASSERT_EQUAL_INT(JCE_ENGINE_CONSTRUCTING, boot.owner_state);
    boot.headless = true; calls.fail_headless = true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, actual_bootstrap(&boot));
    TEST_ASSERT_EQUAL_UINT(2, calls.failed); TEST_ASSERT_EQUAL_UINT(0, calls.bind);
}
static void test_noop_normal_marks_presentation_unavailable(void)
{
    calls.backend = JCE_BACKEND_NOOP;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, actual_bootstrap(&boot)); assert_bound(false);
}
static void test_absent_policy_leaves_each_boot_path_unbound(void)
{
    boot.launch_policy = (JceEngineLaunchPolicy){0};
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, actual_bootstrap(&boot));
    calls.fail_normal = true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, actual_bootstrap(&boot));
    boot.headless = true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, actual_bootstrap(&boot));
    TEST_ASSERT_FALSE(boot.launch_policy_bound); TEST_ASSERT_NULL(s_frame_cap.launch_owner);
    TEST_ASSERT_EQUAL_UINT(0, calls.bind); TEST_ASSERT_EQUAL_INT(144, jce_engine_get_target_fps());
}
static void test_foreign_fps_session_refuses_all_boot_paths_before_vsync_bind(void)
{
    TEST_ASSERT_TRUE(jce_engine_launch_fps_begin(&normal_token, true, 600));
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, actual_bootstrap(&boot));
    calls.fail_normal = true; TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, actual_bootstrap(&boot));
    boot.headless = true; TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, actual_bootstrap(&boot));
    jce_engine_launch_fps_end(&normal_token);
    TEST_ASSERT_EQUAL_UINT(3, calls.failed); TEST_ASSERT_EQUAL_UINT(0, calls.bind);
    TEST_ASSERT_EQUAL_UINT(0, calls.warning); TEST_ASSERT_FALSE(boot.launch_policy_bound);
}

static void test_noninteractive_fallback_is_ready_bounded_and_observable(void)
{
    calls.fail_normal=true;boot.hidden_window=true;budget_frames="3";budget_seconds="0.25";
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,actual_bootstrap(&boot));
    TEST_ASSERT_EQUAL_UINT(0,calls.warning);TEST_ASSERT_EQUAL_UINT(1,calls.warning_logs);
    TEST_ASSERT_EQUAL_UINT(3,boot.max_frames);TEST_ASSERT_EQUAL_DOUBLE(.25,boot.max_seconds);
    TEST_ASSERT_EQUAL_UINT64(boot.frame_counter_prev,boot.run_start_counter);
    TEST_ASSERT_EQUAL_INT(JCE_ENGINE_READY,boot.owner_state);
    TEST_ASSERT_FALSE(boot.constructor_active);TEST_ASSERT_EQUAL_UINT(1,calls.clock);
}
static void test_not_focusable_fallback_keeps_warning_without_modal(void)
{
    calls.fail_normal=true;calls.not_focusable=true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,actual_bootstrap(&boot));
    TEST_ASSERT_EQUAL_UINT(0,calls.warning);TEST_ASSERT_EQUAL_UINT(1,calls.warning_logs);
}
static void test_interactive_fallback_keeps_one_exact_warning(void)
{
    calls.fail_normal=true;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,actual_bootstrap(&boot));
    TEST_ASSERT_EQUAL_UINT(1,calls.warning);TEST_ASSERT_EQUAL_UINT(1,calls.warning_logs);
}
static void test_common_budget_precedes_headless_and_normal_clock_stays_late(void)
{
    budget_frames="4";budget_seconds="0.5";boot.run_start_counter=777;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,actual_bootstrap(&boot));
    TEST_ASSERT_EQUAL_UINT(4,boot.max_frames);TEST_ASSERT_EQUAL_DOUBLE(.5,boot.max_seconds);
    TEST_ASSERT_EQUAL_UINT64(777,boot.run_start_counter);
    actual_late_run_clock(&boot);TEST_ASSERT_EQUAL_UINT64(2000,boot.run_start_counter);
    /* The next constructor represents a new session, with its real prior owner retired. */
    jce_engine_launch_fps_end(&boot);boot.launch_policy_bound=false;
    boot.headless=true;boot.max_frames=0;
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,actual_bootstrap(&boot));
    TEST_ASSERT_EQUAL_UINT(4,boot.max_frames);TEST_ASSERT_EQUAL_UINT(4,calls.env_reads);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_normal_binds_once_and_resolves_initial_config);
    RUN_TEST(test_headless_binds_once_then_preserves_original_join);
    RUN_TEST(test_fallback_binds_before_its_existing_early_return);
    RUN_TEST(test_creation_failures_never_bind_or_claim_successful_join);
    RUN_TEST(test_noop_normal_marks_presentation_unavailable);
    RUN_TEST(test_absent_policy_leaves_each_boot_path_unbound);
    RUN_TEST(test_foreign_fps_session_refuses_all_boot_paths_before_vsync_bind);
    RUN_TEST(test_noninteractive_fallback_is_ready_bounded_and_observable);
    RUN_TEST(test_not_focusable_fallback_keeps_warning_without_modal);
    RUN_TEST(test_interactive_fallback_keeps_one_exact_warning);
    RUN_TEST(test_common_budget_precedes_headless_and_normal_clock_stays_late);
    return UNITY_END();
}
