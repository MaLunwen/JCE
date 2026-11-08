/* Complete SDL process owner with actual property bags and typed counted
 * Windows/process ports. Invalid inherited handles refuse spawn. No native
 * process is run here; separate GUI/public-SDK controls prove OS behavior. */
#include "unity.h"
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_log.h>
#include "os/core/jce_memory.h"
#include "os/core/jce_trace_scope_internal.h"
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
static unsigned char process_tag,stdout_tag,stderr_tag;
static SDL_PropertiesID child_props;
static JceProcess *active;
static struct {
    HANDLE output_handle,error_handle;
    bool output_valid,error_valid,no_properties,spawn_fail,wrapper_oom,overwrite_error;
    bool running,kill_success;
    int exit_code;
    unsigned output_queries,error_queries,validity_queries,properties_create,properties_destroy;
    unsigned spawn,process_properties,destroy,wait_poll,wait_block,kill,force,alloc,free;
    unsigned reads,closed_output,closed_error,logs;
    SDL_ProcessIO input_mode,output_mode,error_mode;
    char executable[128],first_arg[128],error[128],log[512];
    size_t output_offset,error_offset;
} port;
static HANDLE WINAPI fixture_standard_handle(DWORD stream)
{if(stream==STD_OUTPUT_HANDLE){++port.output_queries;return port.output_handle;}
 TEST_ASSERT_EQUAL_UINT32(STD_ERROR_HANDLE,stream);++port.error_queries;return port.error_handle;}
static BOOL WINAPI fixture_handle_information(HANDLE handle,LPDWORD flags)
{++port.validity_queries;*flags=0; /* files/pipes need not be console or originally inheritable */
 return handle==port.output_handle ? port.output_valid : port.error_valid;}
#define GetStdHandle fixture_standard_handle
#define GetHandleInformation fixture_handle_information
#ifndef JCE_TEST_PROCESS_STDIO_POLICY
#define JCE_TEST_PROCESS_STDIO_POLICY "os/core/jce_process_stdio_internal.h"
#endif
#include JCE_TEST_PROCESS_STDIO_POLICY
#undef GetStdHandle
#undef GetHandleInformation
static SDL_PropertiesID SDLCALL fixture_create_properties(void)
{++port.properties_create;return port.no_properties ? 0 : SDL_CreateProperties();}
static void SDLCALL fixture_destroy_properties(SDL_PropertiesID props)
{++port.properties_destroy;SDL_DestroyProperties(props);
 if(port.overwrite_error)SDL_SetError("cleanup changed the spawn error");}
static bool inherited_valid(bool error)
{HANDLE handle=error ? port.error_handle : port.output_handle;
 return handle && handle!=INVALID_HANDLE_VALUE && (error ? port.error_valid : port.output_valid);}
static SDL_Process *SDLCALL fixture_spawn(SDL_PropertiesID props)
{
    ++port.spawn;
    const char *const *argv=SDL_GetPointerProperty(props,SDL_PROP_PROCESS_CREATE_ARGS_POINTER,NULL);
    TEST_ASSERT_NOT_NULL(argv);TEST_ASSERT_NOT_NULL(argv[0]);
    SDL_strlcpy(port.executable,argv[0],sizeof port.executable);
    SDL_strlcpy(port.first_arg,argv[1] ? argv[1] : "",sizeof port.first_arg);
    TEST_ASSERT_EQUAL_STRING("private-token",argv[2]);TEST_ASSERT_NULL(argv[3]);
    port.input_mode=(SDL_ProcessIO)SDL_GetNumberProperty(props,SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,-1);
    port.output_mode=(SDL_ProcessIO)SDL_GetNumberProperty(props,SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,-1);
    port.error_mode=(SDL_ProcessIO)SDL_GetNumberProperty(props,SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,-1);
    TEST_ASSERT_EQUAL_INT(SDL_PROCESS_STDIO_NULL,port.input_mode);
    TEST_ASSERT_FALSE(SDL_GetBooleanProperty(props,SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN,false));
    if(port.spawn_fail || (port.output_mode==SDL_PROCESS_STDIO_INHERITED && !inherited_valid(false)) ||
       (port.error_mode==SDL_PROCESS_STDIO_INHERITED && !inherited_valid(true))) {
        SDL_SetError("DuplicateHandle invalid inherited stream");return NULL;
    }
    if(port.output_mode==SDL_PROCESS_STDIO_APP)SDL_SetPointerProperty(child_props,SDL_PROP_PROCESS_STDOUT_POINTER,&stdout_tag);
    if(port.error_mode==SDL_PROCESS_STDIO_APP)SDL_SetPointerProperty(child_props,SDL_PROP_PROCESS_STDERR_POINTER,&stderr_tag);
    return (SDL_Process *)&process_tag;
}
static SDL_PropertiesID SDLCALL fixture_process_properties(SDL_Process *process)
{TEST_ASSERT_EQUAL_PTR(&process_tag,process);++port.process_properties;return child_props;}
static size_t SDLCALL fixture_read(SDL_IOStream *stream,void *buffer,size_t cap)
{++port.reads;bool error=stream==(SDL_IOStream *)&stderr_tag;
 TEST_ASSERT_TRUE(error || stream==(SDL_IOStream *)&stdout_tag);
 const char *text=error ? "stderr-payload\n" : "stdout-payload\n";
 size_t *offset=error ? &port.error_offset : &port.output_offset;
 size_t count=strlen(text)-*offset;if(count>cap)count=cap;memcpy(buffer,text+*offset,count);*offset+=count;return count;}
static bool SDLCALL fixture_wait(SDL_Process *process,bool block,int *code)
{TEST_ASSERT_EQUAL_PTR(&process_tag,process);if(block)++port.wait_block;else ++port.wait_poll;
 if(port.running)return false;*code=port.exit_code;return true;}
static bool SDLCALL fixture_kill(SDL_Process *process,bool force)
{TEST_ASSERT_EQUAL_PTR(&process_tag,process);++port.kill;if(force){++port.force;if(port.kill_success)port.running=false;}
 return port.kill_success;}
static void SDLCALL fixture_destroy(SDL_Process *process)
{TEST_ASSERT_EQUAL_PTR(&process_tag,process);++port.destroy;
 if(port.output_mode==SDL_PROCESS_STDIO_APP)++port.closed_output;
 if(port.error_mode==SDL_PROCESS_STDIO_APP)++port.closed_error;}
static void *fixture_alloc(size_t count,size_t size)
{++port.alloc;return port.wrapper_oom ? NULL : jce__calloc_tracked(count,size);}
static void fixture_free(void *pointer){if(pointer)++port.free;jce__free_tracked(pointer);}
static void JCE_CALL fixture_log(JceLogLevel level,const char *tag,const char *file,int line,const char *format,...)
{(void)level;(void)tag;(void)file;(void)line;++port.logs;va_list args;va_start(args,format);
 vsnprintf(port.log,sizeof port.log,format,args);va_end(args);}
#define SDL_CreateProperties fixture_create_properties
#define SDL_DestroyProperties fixture_destroy_properties
#define SDL_CreateProcessWithProperties fixture_spawn
#define SDL_GetProcessProperties fixture_process_properties
#define SDL_ReadIO fixture_read
#define SDL_WaitProcess fixture_wait
#define SDL_KillProcess fixture_kill
#define SDL_DestroyProcess fixture_destroy
#define jce__calloc_tracked fixture_alloc
#define jce__free_tracked fixture_free
#define jce_log_write fixture_log
#ifndef JCE_TEST_PROCESS_OWNER
#define JCE_TEST_PROCESS_OWNER "os/core/jce_process.c"
#endif
#include JCE_TEST_PROCESS_OWNER
#undef SDL_CreateProperties
#undef SDL_DestroyProperties
#undef SDL_CreateProcessWithProperties
#undef SDL_GetProcessProperties
#undef SDL_ReadIO
#undef SDL_WaitProcess
#undef SDL_KillProcess
#undef SDL_DestroyProcess
#undef jce__calloc_tracked
#undef jce__free_tracked
#undef jce_log_write
void setUp(void)
{
    jce_trace_set_enabled(false);jce_trace_reset();
    memset(&port,0,sizeof port);port.output_handle=(HANDLE)(uintptr_t)17;port.error_handle=(HANDLE)(uintptr_t)23;
    port.output_valid=port.error_valid=true;port.running=true;port.kill_success=true;port.exit_code=23;
    active=NULL;child_props=SDL_CreateProperties();TEST_ASSERT_NOT_EQUAL_UINT(0,child_props);SDL_ClearError();
}
void tearDown(void)
{if(active){jce_process_destroy(active);active=NULL;}SDL_DestroyProperties(child_props);child_props=0;
 jce_trace_thread_unregister();jce_trace_set_enabled(false);}
static JceProcessConfig config(bool output,bool error)
{JceProcessConfig cfg={"child.exe",NULL,"\"literal with spaces\" private-token",output,error};return cfg;}
static void absent_handles_preserve_all_four_capture_combinations(void)
{
    port.output_handle=NULL;port.error_handle=INVALID_HANDLE_VALUE;
    for(unsigned bits=0;bits<4;++bits){JceProcessConfig cfg=config((bits&1)!=0,(bits&2)!=0);
        active=jce_process_spawn(&cfg);TEST_ASSERT_NOT_NULL(active);
        TEST_ASSERT_EQUAL_INT(cfg.capture_stdout ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL,port.output_mode);
        TEST_ASSERT_EQUAL_INT(cfg.capture_stderr ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL,port.error_mode);
        TEST_ASSERT_EQUAL_STRING("literal with spaces",port.first_arg);
        jce_process_destroy(active);active=NULL;port.running=true;
    }
    TEST_ASSERT_EQUAL_UINT(2,port.output_queries);TEST_ASSERT_EQUAL_UINT(2,port.error_queries);
    TEST_ASSERT_EQUAL_UINT(0,port.validity_queries);TEST_ASSERT_EQUAL_UINT(4,port.destroy);
    TEST_ASSERT_EQUAL_UINT(4,port.properties_destroy);TEST_ASSERT_EQUAL_UINT(4,port.free);
}
static void stale_and_partial_handles_discard_only_the_unavailable_stream(void)
{
    port.error_valid=false;JceProcessConfig cfg=config(false,false);active=jce_process_spawn(&cfg);
    TEST_ASSERT_NOT_NULL(active);TEST_ASSERT_EQUAL_INT(SDL_PROCESS_STDIO_INHERITED,port.output_mode);
    TEST_ASSERT_EQUAL_INT(SDL_PROCESS_STDIO_NULL,port.error_mode);TEST_ASSERT_EQUAL_UINT(2,port.validity_queries);
    jce_process_destroy(active);active=NULL;port.running=true;
    port.output_handle=NULL;port.error_valid=true;active=jce_process_spawn(&cfg);TEST_ASSERT_NOT_NULL(active);
    TEST_ASSERT_EQUAL_INT(SDL_PROCESS_STDIO_NULL,port.output_mode);TEST_ASSERT_EQUAL_INT(SDL_PROCESS_STDIO_INHERITED,port.error_mode);
    TEST_ASSERT_EQUAL_UINT(3,port.validity_queries);
}
static void valid_redirected_handles_keep_inheritance_and_capture_skips_observation(void)
{
    JceProcessConfig cfg=config(false,false);active=jce_process_spawn(&cfg);TEST_ASSERT_NOT_NULL(active);
    TEST_ASSERT_EQUAL_INT(SDL_PROCESS_STDIO_INHERITED,port.output_mode);TEST_ASSERT_EQUAL_INT(SDL_PROCESS_STDIO_INHERITED,port.error_mode);
    TEST_ASSERT_EQUAL_UINT(2,port.validity_queries);jce_process_destroy(active);active=NULL;port.running=true;
    cfg=config(true,true);active=jce_process_spawn(&cfg);TEST_ASSERT_NOT_NULL(active);
    TEST_ASSERT_EQUAL_UINT(1,port.output_queries);TEST_ASSERT_EQUAL_UINT(1,port.error_queries);
    TEST_ASSERT_EQUAL_INT(SDL_PROCESS_STDIO_APP,port.output_mode);TEST_ASSERT_EQUAL_INT(SDL_PROCESS_STDIO_APP,port.error_mode);
}
static void captured_bytes_and_real_exit_code_survive_poll_and_destroy(void)
{
    JceProcessConfig cfg=config(true,true);active=jce_process_spawn(&cfg);TEST_ASSERT_NOT_NULL(active);
    char buffer[32]={0};TEST_ASSERT_EQUAL_UINT(15,jce_process_read_stdout(active,buffer,sizeof buffer));
    TEST_ASSERT_EQUAL_STRING("stdout-payload\n",buffer);memset(buffer,0,sizeof buffer);
    TEST_ASSERT_EQUAL_UINT(15,jce_process_read_stderr(active,buffer,sizeof buffer));TEST_ASSERT_EQUAL_STRING("stderr-payload\n",buffer);
    int code=-91;TEST_ASSERT_FALSE(jce_process_poll_exit(active,&code));TEST_ASSERT_EQUAL_INT(-91,code);
    port.running=false;TEST_ASSERT_TRUE(jce_process_poll_exit(active,&code));TEST_ASSERT_EQUAL_INT(23,code);
    jce_process_destroy(active);active=NULL;TEST_ASSERT_EQUAL_UINT(0,port.kill);TEST_ASSERT_EQUAL_UINT(1,port.destroy);
    TEST_ASSERT_EQUAL_UINT(1,port.closed_output);TEST_ASSERT_EQUAL_UINT(1,port.closed_error);TEST_ASSERT_EQUAL_UINT(1,port.free);
}
static void uncaptured_reads_do_not_add_discard_pipes_and_running_destroy_kills_and_reaps(void)
{
    port.output_handle=port.error_handle=NULL;JceProcessConfig cfg=config(false,false);
    active=jce_process_spawn(&cfg);TEST_ASSERT_NOT_NULL(active);char buffer[8];
    TEST_ASSERT_EQUAL_UINT(0,jce_process_read_stdout(active,buffer,sizeof buffer));
    TEST_ASSERT_EQUAL_UINT(0,jce_process_read_stderr(active,buffer,sizeof buffer));TEST_ASSERT_EQUAL_UINT(0,port.reads);
    TEST_ASSERT_TRUE(jce_process_request_stop(active));TEST_ASSERT_TRUE(jce_process_force_kill(active));
    port.running=true;jce_process_destroy(active);active=NULL;
    TEST_ASSERT_EQUAL_UINT(3,port.kill);TEST_ASSERT_EQUAL_UINT(2,port.force);TEST_ASSERT_EQUAL_UINT(1,port.wait_block);
    TEST_ASSERT_EQUAL_UINT(1,port.destroy);TEST_ASSERT_EQUAL_UINT(0,port.closed_output);TEST_ASSERT_EQUAL_UINT(0,port.closed_error);
}
static void spawn_error_is_owned_before_properties_cleanup_and_never_logs_arguments(void)
{
    JceProcessConfig cfg=config(true,true);port.spawn_fail=port.overwrite_error=true;
    active=jce_process_spawn(&cfg);TEST_ASSERT_NULL(active);
    TEST_ASSERT_EQUAL_STRING("DuplicateHandle invalid inherited stream",jce_process_get_last_spawn_error());
    TEST_ASSERT_EQUAL_UINT(1,port.properties_destroy);TEST_ASSERT_EQUAL_UINT(0,port.destroy);TEST_ASSERT_EQUAL_UINT(0,port.alloc);
    TEST_ASSERT_NULL(strstr(port.log,"private-token"));TEST_ASSERT_NULL(strstr(port.log,"literal with spaces"));
}
static void refusal_and_wrapper_oom_release_only_acquired_owners(void)
{
    TEST_ASSERT_NULL(jce_process_spawn(NULL));JceProcessConfig cfg=config(true,true);cfg.executable_path="";
    TEST_ASSERT_NULL(jce_process_spawn(&cfg));TEST_ASSERT_EQUAL_UINT(0,port.spawn);TEST_ASSERT_EQUAL_UINT(0,port.properties_create);
    cfg=config(true,true);port.no_properties=true;TEST_ASSERT_NULL(jce_process_spawn(&cfg));
    TEST_ASSERT_EQUAL_UINT(0,port.spawn);TEST_ASSERT_EQUAL_UINT(0,port.properties_destroy);
    port.no_properties=false;port.wrapper_oom=true;TEST_ASSERT_NULL(jce_process_spawn(&cfg));
    TEST_ASSERT_EQUAL_UINT(1,port.kill);TEST_ASSERT_EQUAL_UINT(1,port.force);TEST_ASSERT_EQUAL_UINT(1,port.destroy);
    TEST_ASSERT_EQUAL_UINT(1,port.closed_output);TEST_ASSERT_EQUAL_UINT(1,port.closed_error);TEST_ASSERT_EQUAL_UINT(0,port.free);
}
static void actual_missing_stdio_diagnostic_and_spawn_trace_are_bounded_and_correlated(void)
{
    jce_trace_set_enabled(true);jce_trace_thread_register("process-fixture");
    port.output_handle=NULL;port.spawn_fail=true;JceProcessConfig cfg=config(false,true);
    TEST_ASSERT_NULL(jce_process_spawn(&cfg));TEST_ASSERT_EQUAL_UINT(1,port.logs);
    TEST_ASSERT_NOT_NULL(strstr(port.log,"stdout=discard_absent stderr=app spawn=failed"));
    TEST_ASSERT_NULL(strstr(port.log,"private-token"));TEST_ASSERT_NULL(strstr(port.log,"literal with spaces"));
    JceTraceCursor cursor;jce_trace_cursor_init(&cursor);JceTraceEvent events[32];
    uint32_t count=jce_trace_read(&cursor,events,32);unsigned begins=0,ends=0,counters=0;uint64_t id=0;
    for(uint32_t i=0;i<count;++i){JceTraceEvent *event=&events[i];
        if(!strcmp(event->name,"process.spawn") && event->type==JCE_TRACE_EVENT_TASK_BEGIN){id=event->id;++begins;}
        if(!strcmp(event->name,"process.spawn") && event->type==JCE_TRACE_EVENT_TASK_END){++ends;
            TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED,event->value_u32[0]);TEST_ASSERT_EQUAL_UINT64(id,event->id);}
        if(event->type==JCE_TRACE_EVENT_COUNTER && !strncmp(event->name,"process.stdio.",14)){
            ++counters;TEST_ASSERT_EQUAL_UINT64(id,event->id);TEST_ASSERT_EQUAL_UINT64(id,event->parent_id);
            if(!strcmp(event->name,"process.stdio.stdout.mode"))TEST_ASSERT_EQUAL_UINT64(SDL_PROCESS_STDIO_NULL,event->value_u64);
            if(!strcmp(event->name,"process.stdio.stderr.mode"))TEST_ASSERT_EQUAL_UINT64(SDL_PROCESS_STDIO_APP,event->value_u64);
        }
    }
    TEST_ASSERT_EQUAL_UINT(1,begins);TEST_ASSERT_EQUAL_UINT(1,ends);TEST_ASSERT_EQUAL_UINT(4,counters);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(absent_handles_preserve_all_four_capture_combinations);
    RUN_TEST(stale_and_partial_handles_discard_only_the_unavailable_stream);
    RUN_TEST(valid_redirected_handles_keep_inheritance_and_capture_skips_observation);
    RUN_TEST(captured_bytes_and_real_exit_code_survive_poll_and_destroy);
    RUN_TEST(uncaptured_reads_do_not_add_discard_pipes_and_running_destroy_kills_and_reaps);
    RUN_TEST(spawn_error_is_owned_before_properties_cleanup_and_never_logs_arguments);
    RUN_TEST(refusal_and_wrapper_oom_release_only_acquired_owners);
    RUN_TEST(actual_missing_stdio_diagnostic_and_spawn_trace_are_bounded_and_correlated);
    return UNITY_END();
}
