/* Exact Windows lock/publication/activation owner with counted Win ports.
 * No real mutex, HWND activation, mapping or native UI is created. JCE_DIST
 * removes INFO macros; the explicit once diagnostics must remain visible. */
#include "unity.h"
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/platform/jce_single_instance.h>
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static union { uint64_t alignment; unsigned char bytes[64]; } mapping_bytes;
static struct {
    DWORD error, actual_pid, thread_id;
    bool no_mutex, no_mapping, no_view, live_window, iconic, diag, perf;
    bool defer_cleanup, cleanup_same_owner, constructor_active_at_cleanup;
    unsigned creates, mapping_creates, mapping_opens, maps, unmaps, closes, releases;
    unsigned pid_queries, restores, foregrounds, flashes, logs, quits, frees;
    unsigned cleanup_attempts;
    JceAppResult quit_result, gate_result;
    JceEngine *expected_owner;
    JceLogLevel level;
    char log[1024];
} port;
static HANDLE WINAPI counted_mutex(LPSECURITY_ATTRIBUTES a,BOOL owner,LPCSTR name)
{ (void)a; (void)owner; (void)name; ++port.creates; return port.no_mutex ? NULL : (HANDLE)(uintptr_t)1; }
static DWORD WINAPI counted_error(void) { return port.error; }
static BOOL WINAPI counted_close(HANDLE h) { (void)h; ++port.closes; return TRUE; }
static BOOL WINAPI counted_release(HANDLE h) { (void)h; ++port.releases; return TRUE; }
static HANDLE WINAPI counted_create_mapping(HANDLE f,LPSECURITY_ATTRIBUTES a,DWORD p,DWORD hi,DWORD lo,LPCSTR name)
{ (void)f;(void)a;(void)p;(void)hi;(void)lo;(void)name;++port.mapping_creates;return (HANDLE)(uintptr_t)2; }
static HANDLE WINAPI counted_open_mapping(DWORD rights,BOOL inherit,LPCSTR name)
{ (void)rights;(void)inherit;(void)name;++port.mapping_opens;return port.no_mapping ? NULL : (HANDLE)(uintptr_t)2; }
static LPVOID WINAPI counted_map(HANDLE h,DWORD rights,DWORD hi,DWORD lo,SIZE_T bytes)
{ (void)h;(void)rights;(void)hi;(void)lo;(void)bytes;++port.maps;return port.no_view ? NULL : mapping_bytes.bytes; }
static BOOL WINAPI counted_unmap(LPCVOID memory) { (void)memory;++port.unmaps;return TRUE; }
static DWORD WINAPI counted_current_pid(void) { return 42; }
static BOOL WINAPI counted_is_window(HWND h) { (void)h;return port.live_window ? TRUE : FALSE; }
static DWORD WINAPI counted_window_pid(HWND h,LPDWORD pid)
{ (void)h;++port.pid_queries;*pid=port.actual_pid;return port.thread_id; }
static BOOL WINAPI counted_is_iconic(HWND h) { (void)h;return port.iconic ? TRUE : FALSE; }
static BOOL WINAPI counted_show(HWND h,int show) { (void)h;(void)show;++port.restores;return TRUE; }
static BOOL WINAPI counted_foreground(HWND h) { (void)h;++port.foregrounds;return FALSE; }
static BOOL WINAPI counted_flash(PFLASHWINFO info) { (void)info;++port.flashes;return FALSE; }
static char *counted_getenv(const char *name)
{ return (port.diag && strcmp(name,"JCE_EDITOR_CADENCE_DIAG")==0) ||
    (port.perf && strcmp(name,"JCE_PERF_LOG")==0) ? "1" : NULL; }
static void JCE_CALL counted_log(JceLogLevel level,const char *tag,const char *file,int line,const char *format,...)
{ (void)tag;(void)file;(void)line;++port.logs;port.level=level;
  va_list args;va_start(args,format);vsnprintf(port.log,sizeof port.log,format,args);va_end(args); }
#define CreateMutexA counted_mutex
#define GetLastError counted_error
#define CloseHandle counted_close
#define ReleaseMutex counted_release
#define CreateFileMappingA counted_create_mapping
#define OpenFileMappingA counted_open_mapping
#define MapViewOfFile counted_map
#define UnmapViewOfFile counted_unmap
#define GetCurrentProcessId counted_current_pid
#define IsWindow counted_is_window
#define GetWindowThreadProcessId counted_window_pid
#define IsIconic counted_is_iconic
#define ShowWindow counted_show
#define SetForegroundWindow counted_foreground
#define FlashWindowEx counted_flash
#define getenv counted_getenv
#define jce_log_write counted_log
#ifndef JCE_TEST_SINGLE_INSTANCE_OWNER
#define JCE_TEST_SINGLE_INSTANCE_OWNER "os/platform/jce_single_instance.c"
#endif
#include JCE_TEST_SINGLE_INSTANCE_OWNER
/* Consume the actual rejection branch AND shared constructor failure tail.
 * The typed cleanup port counts delegation/completion, not real SDL shutdown
 * or heap release. Complete Engine ownership is tested under application/. */
#undef LOG_TAG
#define LOG_TAG "engine"
struct JceEngine {
    struct { char window_title[32]; } config;
    bool constructor_active;
};
static JceEngine gate_object;
static JceEngine *gate_owner;
static void JCE_CALL counted_request_quit(JceAppResult result)
{ ++port.quits;port.quit_result=result; }
static bool JCE_CALL counted_try_destroy(JceEngine **owner)
{
    ++port.cleanup_attempts;
    port.cleanup_same_owner=owner && *owner==port.expected_owner;
    port.constructor_active_at_cleanup=owner && *owner && (*owner)->constructor_active;
    if (!owner || !*owner || port.defer_cleanup) return false;
    ++port.frees;*owner=NULL;
    return true;
}
#define jce_engine_request_quit_with_result counted_request_quit
#define jce_engine_try_destroy counted_try_destroy
static JceAppResult actual_engine_instance_gate_result(bool allow_multiple_instances,
                                                       bool not_focusable,JceEngine **owner)
{
    JceEngine *e=*owner;
#include JCE_TEST_SINGLE_INSTANCE_ENGINE_BLOCK
    return JCE_APP_CONTINUE;
#include JCE_TEST_SINGLE_INSTANCE_ENGINE_FAILURE
}
static void *actual_engine_instance_gate(bool allow_multiple_instances,bool not_focusable)
{
    memset(&gate_object,0,sizeof gate_object);
    memcpy(gate_object.config.window_title,"Fixture App",sizeof "Fixture App");
    gate_object.constructor_active=true;
    gate_owner=&gate_object;
    port.expected_owner=gate_owner;
    port.gate_result=actual_engine_instance_gate_result(allow_multiple_instances,
                                                       not_focusable,&gate_owner);
    return port.gate_result==JCE_APP_CONTINUE ? gate_owner : NULL;
}
static void reset_ports(void)
{
    memset(&port,0,sizeof port); memset(&mapping_bytes,0,sizeof mapping_bytes);
    gate_owner=NULL;
    port.actual_pid=42;port.thread_id=9;port.live_window=true;
    ((SiWndPayload*)mapping_bytes.bytes)->pid=42;
    ((SiWndPayload*)mapping_bytes.bytes)->hwnd=0x1234;
}
void setUp(void) { jce_single_instance_unlock();reset_ports(); }
void tearDown(void) { jce_single_instance_unlock();TEST_ASSERT_FALSE(jce_single_instance_is_locked()); }
static void test_matching_pid_allows_only_best_effort_attention(void)
{
    port.iconic=true;
    TEST_ASSERT_TRUE(jce_single_instance_activate_existing());
    TEST_ASSERT_EQUAL_UINT(1,port.pid_queries);
    TEST_ASSERT_EQUAL_UINT(1,port.unmaps);TEST_ASSERT_EQUAL_UINT(1,port.closes);
    TEST_ASSERT_EQUAL_UINT(1,port.restores);TEST_ASSERT_EQUAL_UINT(1,port.foregrounds);
    TEST_ASSERT_EQUAL_UINT(1,port.flashes);
    /* FALSE foreground/flash returns do not certify actual native focus. */
}
static void test_recycled_hwnd_for_other_pid_never_receives_attention(void)
{
    port.actual_pid=77;port.iconic=true;
    TEST_ASSERT_FALSE(jce_single_instance_activate_existing());
    TEST_ASSERT_EQUAL_UINT(1,port.pid_queries);
    TEST_ASSERT_EQUAL_UINT(0,port.restores);TEST_ASSERT_EQUAL_UINT(0,port.foregrounds);
    TEST_ASSERT_EQUAL_UINT(0,port.flashes);
    TEST_ASSERT_EQUAL_UINT(1,port.unmaps);TEST_ASSERT_EQUAL_UINT(1,port.closes);
}
static void test_missing_pid_and_failed_pid_lookup_refuse_attention(void)
{
    ((SiWndPayload*)mapping_bytes.bytes)->pid=0;
    TEST_ASSERT_FALSE(jce_single_instance_activate_existing());
    ((SiWndPayload*)mapping_bytes.bytes)->pid=42;
    port.actual_pid=0;
    TEST_ASSERT_FALSE(jce_single_instance_activate_existing());
    port.actual_pid=42;port.thread_id=0;
    TEST_ASSERT_FALSE(jce_single_instance_activate_existing());
    TEST_ASSERT_EQUAL_UINT(0,port.restores);TEST_ASSERT_EQUAL_UINT(0,port.foregrounds);
    TEST_ASSERT_EQUAL_UINT(0,port.flashes);
}
static void test_absent_mapping_map_failure_and_stale_window_close_only_owned_handles(void)
{
    port.no_mapping=true;
    TEST_ASSERT_FALSE(jce_single_instance_activate_existing());
    TEST_ASSERT_EQUAL_UINT(0,port.closes);
    port.no_mapping=false;port.no_view=true;
    TEST_ASSERT_FALSE(jce_single_instance_activate_existing());
    TEST_ASSERT_EQUAL_UINT(1,port.closes);TEST_ASSERT_EQUAL_UINT(0,port.unmaps);
    port.no_view=false;port.live_window=false;
    TEST_ASSERT_FALSE(jce_single_instance_activate_existing());
    TEST_ASSERT_EQUAL_UINT(2,port.closes);TEST_ASSERT_EQUAL_UINT(1,port.unmaps);
    TEST_ASSERT_EQUAL_UINT(0,port.pid_queries);TEST_ASSERT_EQUAL_UINT(0,port.foregrounds);
}
static void test_lock_diagnostics_are_optin_and_visible_with_dist_info_stripped(void)
{
    TEST_ASSERT_TRUE(jce_single_instance_lock("Fixture App"));
    TEST_ASSERT_EQUAL_UINT(0,port.logs);
    jce_single_instance_unlock();reset_ports();port.diag=true;
    TEST_ASSERT_TRUE(jce_single_instance_lock("Fixture App"));
    TEST_ASSERT_EQUAL_UINT(1,port.logs);TEST_ASSERT_EQUAL(JCE_LOG_LEVEL_INFO,port.level);
    TEST_ASSERT_NOT_NULL(strstr(port.log,"stage=lock-acquired"));
    TEST_ASSERT_TRUE(jce_single_instance_lock("Fixture App"));
    TEST_ASSERT_EQUAL_UINT(1,port.creates);TEST_ASSERT_EQUAL_UINT(1,port.logs);
    jce_single_instance_unlock();reset_ports();port.perf=true;port.error=ERROR_ALREADY_EXISTS;
    TEST_ASSERT_FALSE(jce_single_instance_lock("Fixture App"));
    TEST_ASSERT_EQUAL_UINT(1,port.logs);TEST_ASSERT_NOT_NULL(strstr(port.log,"stage=lock-not-acquired"));
    TEST_ASSERT_EQUAL_UINT(1,port.closes);TEST_ASSERT_FALSE(jce_single_instance_is_locked());
}
static void test_os_lock_error_is_failure_not_owned_duplicate_success(void)
{
    port.no_mutex=true;port.error=ERROR_ACCESS_DENIED;
    TEST_ASSERT_FALSE(jce_single_instance_lock("Fixture App"));
    TEST_ASSERT_FALSE(jce_single_instance_is_locked());
    TEST_ASSERT_EQUAL(JCE_LOG_LEVEL_ERROR,port.level);
    TEST_ASSERT_EQUAL_UINT(0,port.closes);TEST_ASSERT_EQUAL_UINT(0,port.releases);
}
static void test_published_window_keeps_pid_and_releases_mapping_and_mutex_once(void)
{
    TEST_ASSERT_TRUE(jce_single_instance_lock("Fixture App"));
    jce_single_instance_publish_window((void*)(uintptr_t)0x1234);
    jce_single_instance_publish_window((void*)(uintptr_t)0x5678);
    TEST_ASSERT_EQUAL_UINT(42,((SiWndPayload*)mapping_bytes.bytes)->pid);
    TEST_ASSERT_EQUAL_UINT64(0x5678,((SiWndPayload*)mapping_bytes.bytes)->hwnd);
    TEST_ASSERT_EQUAL_UINT(1,port.mapping_creates);TEST_ASSERT_EQUAL_UINT(1,port.maps);
    jce_single_instance_unlock();jce_single_instance_unlock();
    TEST_ASSERT_EQUAL_UINT(1,port.unmaps);TEST_ASSERT_EQUAL_UINT(2,port.closes);
    TEST_ASSERT_EQUAL_UINT(1,port.releases);
}
static void test_actual_startup_rejection_logs_app_stage_attention_and_failure_in_dist(void)
{
    port.error=ERROR_ALREADY_EXISTS;
    TEST_ASSERT_NULL(actual_engine_instance_gate(false,false));
    TEST_ASSERT_EQUAL_UINT(1,port.logs);TEST_ASSERT_EQUAL(JCE_LOG_LEVEL_INFO,port.level);
    TEST_ASSERT_NOT_NULL(strstr(port.log,"stage=single-instance-lock app=Fixture App activation=attempted result=failure"));
    TEST_ASSERT_EQUAL_UINT(1,port.quits);TEST_ASSERT_EQUAL_UINT(1,port.frees);
    TEST_ASSERT_EQUAL(JCE_APP_FAILURE,port.quit_result);
    TEST_ASSERT_EQUAL(JCE_APP_FAILURE,port.gate_result);
    TEST_ASSERT_EQUAL_UINT(1,port.cleanup_attempts);
    TEST_ASSERT_TRUE(port.cleanup_same_owner);
    TEST_ASSERT_FALSE(port.constructor_active_at_cleanup);
    TEST_ASSERT_NULL(gate_owner);
    reset_ports();port.error=ERROR_ALREADY_EXISTS;
    TEST_ASSERT_NULL(actual_engine_instance_gate(false,true));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"activation=not-requested"));
    TEST_ASSERT_EQUAL_UINT(0,port.mapping_opens);TEST_ASSERT_EQUAL_UINT(0,port.foregrounds);
    reset_ports();port.error=ERROR_ALREADY_EXISTS;port.no_mapping=true;
    TEST_ASSERT_NULL(actual_engine_instance_gate(false,false));
    TEST_ASSERT_NOT_NULL(strstr(port.log,"activation=unavailable"));
}
static void test_actual_startup_failure_retains_owner_when_cleanup_port_refuses(void)
{
    port.error=ERROR_ALREADY_EXISTS;port.defer_cleanup=true;
    TEST_ASSERT_NULL(actual_engine_instance_gate(false,false));
    TEST_ASSERT_EQUAL(JCE_APP_FAILURE,port.gate_result);
    TEST_ASSERT_EQUAL(JCE_APP_FAILURE,port.quit_result);
    TEST_ASSERT_EQUAL_UINT(1,port.quits);TEST_ASSERT_EQUAL_UINT(0,port.frees);
    TEST_ASSERT_EQUAL_UINT(1,port.cleanup_attempts);
    TEST_ASSERT_TRUE(port.cleanup_same_owner);
    TEST_ASSERT_FALSE(port.constructor_active_at_cleanup);
    TEST_ASSERT_EQUAL_PTR(&gate_object,gate_owner);
}
static void test_actual_startup_bypass_and_acquired_lock_do_not_report_rejection(void)
{
    TEST_ASSERT_NOT_NULL(actual_engine_instance_gate(true,false));
    TEST_ASSERT_EQUAL_UINT(0,port.creates);TEST_ASSERT_EQUAL_UINT(0,port.logs);
    TEST_ASSERT_NOT_NULL(actual_engine_instance_gate(false,false));
    TEST_ASSERT_TRUE(jce_single_instance_is_locked());
    TEST_ASSERT_EQUAL_UINT(0,port.logs);TEST_ASSERT_EQUAL_UINT(0,port.quits);
    TEST_ASSERT_EQUAL_UINT(0,port.cleanup_attempts);
    TEST_ASSERT_EQUAL_UINT(0,port.frees);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_matching_pid_allows_only_best_effort_attention);
    RUN_TEST(test_recycled_hwnd_for_other_pid_never_receives_attention);
    RUN_TEST(test_missing_pid_and_failed_pid_lookup_refuse_attention);
    RUN_TEST(test_absent_mapping_map_failure_and_stale_window_close_only_owned_handles);
    RUN_TEST(test_lock_diagnostics_are_optin_and_visible_with_dist_info_stripped);
    RUN_TEST(test_os_lock_error_is_failure_not_owned_duplicate_success);
    RUN_TEST(test_published_window_keeps_pid_and_releases_mapping_and_mutex_once);
    RUN_TEST(test_actual_startup_rejection_logs_app_stage_attention_and_failure_in_dist);
    RUN_TEST(test_actual_startup_failure_retains_owner_when_cleanup_port_refuses);
    RUN_TEST(test_actual_startup_bypass_and_acquired_lock_do_not_report_rejection);
    return UNITY_END();
}
