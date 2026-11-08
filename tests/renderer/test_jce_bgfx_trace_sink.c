/* Actual complete callback vtbl with typed DIST logging and metadata ports. */
#include "unity.h"
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_trace.h>
#include <jce/renderer/jce_render_readback.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    char *gfx;
    unsigned logs, metadata;
    JceLogLevel level;
    char message[2048], captured[1024];
} port;
static char *trace_env(const char *name)
{ return !strcmp(name,"JCE_GFX_DEBUG") ? port.gfx : NULL; }
static void trace_log(JceLogLevel level,const char *tag,const char *file,int line,const char *format,...)
{
    TEST_ASSERT_EQUAL_STRING("bgfx",tag);TEST_ASSERT_NOT_NULL(file);TEST_ASSERT_TRUE(line>0);
    TEST_ASSERT_TRUE(!strcmp(format,"%s:%u %s") ||
        !strcmp(format,"[bgfx_trace_truncated required_bytes=%d] %s:%u %s"));
    ++port.logs;port.level=level;va_list args;va_start(args,format);
    vsnprintf(port.message,sizeof(port.message),format,args);va_end(args);
}
/* The complete callback owner also references this unrelated public hook. */
bool jce_render_readback_format_supported(JceRenderFormat format)
{
    (void)format;
    TEST_FAIL_MESSAGE("Trace-only fixture must not request GPU readback");
    return false;
}
uint32_t jce_rcb_host_frame_index(void) { return 0; }
void jce_renderer_caps_api_capture_trace(const char *line)
{ ++port.metadata;snprintf(port.captured,sizeof(port.captured),"%s",line); }
#define getenv trace_env
#define jce_log_write trace_log
#ifndef JCE_BGFX_TRACE_SINK_OWNER
#define JCE_BGFX_TRACE_SINK_OWNER "renderer/jce_renderer_bgfx_callback.c"
#endif
#include JCE_BGFX_TRACE_SINK_OWNER
static bgfx_callback_interface_t *callback;
static void trace_at(const char *path,uint16_t line,const char *format,...)
{
    va_list args;va_start(args,format);
    callback->vtbl->trace_vargs(callback,path,line,format,args);
    va_end(args);
}
void setUp(void)
{
    memset(&port,0,sizeof port);jce_trace_set_enabled(false);
    s_bgfx_trace_enabled=-1;callback=jce_rcb_callback_interface();
}
void tearDown(void) { jce_rcb_after_backend_shutdown(); }
static void default_off_and_zero_prefix_never_write_log(void)
{
    trace_at("renderer_gl.cpp",5184,"BGFX %d: %s",0,"link failed");
    TEST_ASSERT_EQUAL_UINT(0,port.logs+port.metadata);
    s_bgfx_trace_enabled=-1;port.gfx="0";
    trace_at("renderer_gl.cpp",5184,"BGFX disabled");
    TEST_ASSERT_EQUAL_UINT(0,port.logs);
}
static void opt_in_mirrors_info_even_when_dist_macro_erases_info(void)
{
    port.gfx="1";trace_at("renderer_gl.cpp",5184,"BGFX %d: %s",0,"link failed");
    TEST_ASSERT_EQUAL_UINT(1,port.logs);TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_INFO,port.level);
    TEST_ASSERT_EQUAL_STRING("renderer_gl.cpp:5184 BGFX 0: link failed",port.message);
}
static void api_metadata_still_captured_while_debug_log_is_silent(void)
{
    trace_at("renderer_gl.cpp",12,"JCE runtime API version: OpenGL %u.%u\r\n",2u,1u);
    TEST_ASSERT_EQUAL_UINT(1,port.metadata);TEST_ASSERT_EQUAL_UINT(0,port.logs);
    TEST_ASSERT_EQUAL_STRING("JCE runtime API version: OpenGL 2.1",port.captured);
    trace_at("renderer_gl.cpp",13,"JCE runtime shader version: GLSL %s","1.20");
    TEST_ASSERT_EQUAL_UINT(2,port.metadata);TEST_ASSERT_EQUAL_UINT(0,port.logs);
}
static void basename_handles_forward_back_mixed_and_null_paths(void)
{
    port.gfx="1";trace_at("a/b/renderer_gl.cpp",7,"ok");
    TEST_ASSERT_EQUAL_STRING("renderer_gl.cpp:7 ok",port.message);
    trace_at("C:\\src\\renderer_gl.cpp",8,"ok");
    TEST_ASSERT_EQUAL_STRING("renderer_gl.cpp:8 ok",port.message);
    trace_at("C:\\src/sub\\renderer_gl.cpp",9,"ok");
    TEST_ASSERT_EQUAL_STRING("renderer_gl.cpp:9 ok",port.message);
    trace_at(NULL,10,"ok");TEST_ASSERT_EQUAL_STRING("<bgfx>:10 ok",port.message);
}
static void formatting_and_trailing_newlines_do_not_reinterpret_driver_text(void)
{
    port.gfx="1";trace_at("f",5,"value=%d %s\r\n\n",37,"100% ready %n");
    TEST_ASSERT_EQUAL_STRING("f:5 value=37 100% ready %n",port.message);
    TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_INFO,port.level);
}
static void truncation_is_explicit_before_bounded_body_and_exact_fit_is_not_marked(void)
{
    char text[2049];memset(text,'Q',sizeof(text)-1);text[sizeof(text)-1]='\0';
    port.gfx="1";trace_at("f",6,"%s",text);
    TEST_ASSERT_NOT_NULL(strstr(port.message,"[bgfx_trace_truncated required_bytes=2048]"));
    unsigned count=0;for(const char *p=port.message;*p;++p)if(*p=='Q')++count;
    TEST_ASSERT_EQUAL_UINT(1023,count);
    text[1023]='\0';trace_at("f",6,"%s",text);
    TEST_ASSERT_NULL(strstr(port.message,"bgfx_trace_truncated"));
}
static void empty_null_and_blank_messages_stay_silent_but_truncated_blank_is_marked(void)
{
    port.gfx="1";trace_at("f",1,NULL);trace_at("f",1,"");trace_at("f",1,"\r\n\n");
    TEST_ASSERT_EQUAL_UINT(0,port.logs);
    char text[2049];memset(text,'\n',sizeof(text)-1);text[sizeof(text)-1]='\0';
    trace_at("f",1,"%s",text);TEST_ASSERT_EQUAL_UINT(1,port.logs);
    TEST_ASSERT_NOT_NULL(strstr(port.message,"bgfx_trace_truncated required_bytes=2048"));
}
static void selector_remains_latched_and_text_never_changes_device_error_state(void)
{
    trace_at("f",1,"off");port.gfx="1";trace_at("f",1,"still off");
    TEST_ASSERT_EQUAL_UINT(0,port.logs);
    s_bgfx_trace_enabled=-1;jce_atomic_i32_store(&g_rcb_host.device_lost,7);
    trace_at("f",1,"ERROR - FATAL - device lost");
    TEST_ASSERT_EQUAL_UINT(1,port.logs);TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_INFO,port.level);
    TEST_ASSERT_EQUAL_INT32(7,jce_atomic_i32_load(&g_rcb_host.device_lost));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(default_off_and_zero_prefix_never_write_log);
    RUN_TEST(opt_in_mirrors_info_even_when_dist_macro_erases_info);
    RUN_TEST(api_metadata_still_captured_while_debug_log_is_silent);
    RUN_TEST(basename_handles_forward_back_mixed_and_null_paths);
    RUN_TEST(formatting_and_trailing_newlines_do_not_reinterpret_driver_text);
    RUN_TEST(truncation_is_explicit_before_bounded_body_and_exact_fit_is_not_marked);
    RUN_TEST(empty_null_and_blank_messages_stay_silent_but_truncated_blank_is_marked);
    RUN_TEST(selector_remains_latched_and_text_never_changes_device_error_state);
    return UNITY_END();
}
