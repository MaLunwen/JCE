/* Complete shipping SDL/JNI adapters, typed host ports and actual Core heap.
 * Native frees are quarantined until tearDown so old-owner mutants fail an
 * ordinary ownership assertion rather than reading freed test memory. */
#include "unity.h"
#define JCE_NO_APP_DESC_SIZE_SHIM
#include <jce/application/jce_engine.h>
#include <jce/application/jce_app_interface.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_trace.h>
#include <jce/jce_version.h>
#include "os/core/jce_memory.h"
#include <SDL3/SDL.h>
#include <jni.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "application/jce_engine_terminal_internal.h"

/* SDL owns the callback schedule; the fixture supplies its declared context. */
void SDL_AppQuit(void *appstate, SDL_AppResult result);

struct JceEngine { int token; };
static JceEngine engine_token;
static struct {
    JceAppResult create_result, event_result, iterate_result;
    bool create_owner, fail_bridge_alloc, admitted_without_owner, main_thread;
    unsigned creates, events, iterates, destroys, refusals, releases;
    unsigned polls, delayed, delays, exceptions, frees, warnings;
    unsigned properties, fail_property, released_chars;
    bool pending_exception, property_values;
    void *bridge_memory;
    void *sdl_memory;
    JceEngine **owner_slot;
    struct { void *memory; bool freed, sdl; } allocations[16];
    unsigned allocation_count;
    bool reenter_quit, reentered_quit;
    char exception_message[160];
    unsigned acknowledgements;
    bool ack_before_bridge_free, ack_before_sdl_free, fail_ack_write;
    char acknowledgement[768];
} port;
static JceAllocTrack heap_before;
static bool heap_tracking;
static const char *terminal_token = "0123456789abcdef0123456789abcdef";
static const char *terminal_getenv(const char *name)
{
    return strcmp(name, "JCE_TERMINAL_ACK_TOKEN") == 0 ? terminal_token : NULL;
}
static int terminal_print(FILE *stream, const char *format, ...)
{
    if (stream != stderr || strcmp(format, "%s") != 0) return -1;
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(port.acknowledgement, sizeof(port.acknowledgement), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(port.acknowledgement)) return -1;
    ++port.acknowledgements;
    if (strstr(port.acknowledgement, "\"host\":\"jni\"")) {
        for (unsigned i = 0; i < port.allocation_count; ++i)
            if (port.allocations[i].memory == port.bridge_memory && !port.allocations[i].freed)
                port.ack_before_bridge_free = true;
    }
    if (strstr(port.acknowledgement, "\"host\":\"sdl\"")) {
        for (unsigned i = 0; i < port.allocation_count; ++i)
            if (port.allocations[i].memory == port.sdl_memory && !port.allocations[i].freed)
                port.ack_before_sdl_free = true;
    }
    if (port.fail_ack_write) {
        /* fprintf can return negative after a prefix reached the destination. */
        port.acknowledgement[17] = '\0';
        return -1;
    }
    return length;
}
#define getenv terminal_getenv
#define fprintf terminal_print
#include "application/jce_engine_terminal.c"
#undef fprintf
#undef getenv

static JceAppResult port_create(int argc, char **argv, JceEngine **owner)
{
    (void)argc; (void)argv; ++port.creates;
    port.owner_slot = owner;
    *owner = port.create_owner ? &engine_token : NULL;
    if (port.create_owner || port.admitted_without_owner)
        jce_engine_terminal_begin_internal();
    if (port.admitted_without_owner) {
        jce_engine_terminal_log_internal(JCE_LOG_FINAL_CLOSE_NO_FILE);
        jce_engine_terminal_finish_internal();
    }
    return port.create_result;
}
static bool port_destroy(JceEngine **owner)
{
    if (!*owner) return true;
    ++port.destroys;
    if (port.reenter_quit && !port.reentered_quit) {
        port.reentered_quit = true;
        SDL_AppQuit(port.sdl_memory, SDL_APP_FAILURE);
    }
    if (port.refusals) { --port.refusals; return false; }
    *owner = NULL; ++port.releases;
    jce_engine_terminal_log_internal(JCE_LOG_FINAL_CLOSE_NO_FILE);
    jce_engine_terminal_finish_internal();
    return true;
}
static void port_desc(const JceAppDesc *desc) { (void)desc; }
static void port_path(const char *path) { (void)path; }
static bool port_hook(void) { return true; }
static JceAppResult port_event(JceEngine *engine, const void *event)
{
    (void)engine; (void)event; ++port.events; return port.event_result;
}
static JceAppResult port_iterate(JceEngine *engine)
{
    (void)engine; ++port.iterates; return port.iterate_result;
}
static const char *port_getenv(const char *name) { (void)name; return NULL; }
static bool port_is_main(void) { return port.main_thread; }
static void port_delay(uint32_t ms) { ++port.delays; port.delayed += ms; }
static bool port_poll(SDL_Event *event)
{
    (void)event; ++port.polls; return false;
}
static void port_log(JceLogLevel level, const char *tag, const char *file,
                     int line, const char *format, ...)
{
    (void)level; (void)tag; (void)file; (void)line; (void)format; ++port.warnings;
}
static void *port_allocate(size_t count, size_t size, bool sdl)
{
    if (port.fail_bridge_alloc) return NULL;
    TEST_ASSERT_LESS_THAN_UINT(16, port.allocation_count);
    void *memory = JCE_CALLOC(count, size);
    TEST_ASSERT_NOT_NULL(memory);
    port.allocations[port.allocation_count].memory = memory;
    port.allocations[port.allocation_count].sdl = sdl;
    ++port.allocation_count;
    if (sdl) port.sdl_memory = memory;
    else port.bridge_memory = memory;
    return memory;
}
static void *port_calloc(size_t count, size_t size) { return port_allocate(count, size, false); }
static void *port_sdl_calloc(size_t count, size_t size) { return port_allocate(count, size, true); }
static void port_free(void *memory)
{
    if (!memory) return;
    for (unsigned i = 0; i < port.allocation_count; ++i) {
        if (port.allocations[i].memory != memory) continue;
        TEST_ASSERT_FALSE_MESSAGE(port.allocations[i].freed, "host allocation freed twice");
        port.allocations[i].freed = true;
        ++port.frees;
        return;
    }
    TEST_FAIL_MESSAGE("host freed an allocation it does not own");
}
static unsigned port_version(void) { return 0x000D0400u; }
static const char *port_version_string(void) { return "0.13.4"; }

JceAppDesc jce_app_get_desc(void) { JceAppDesc desc = {0}; return desc; }
#define jce_engine_create_owned port_create
#define jce_engine_try_destroy port_destroy
#define jce_engine_set_app_desc port_desc
#define jce_engine_set_config_path port_path
#define jce_engine_set_pak_path port_path
#define jce_engine_event port_event
#define jce_engine_iterate port_iterate
#define jce_alloc_hook_sdl port_hook
#define jce_log_write port_log
#define SDL_getenv port_getenv
#define SDL_IsMainThread port_is_main
#define SDL_Delay port_delay
#define SDL_PollEvent port_poll
#define jce__calloc_tracked port_sdl_calloc
#define jce__free_tracked port_free
#define jce_api_version port_version
#define jce_api_version_string port_version_string
#include JCE_TEST_SDL_HOST_OWNER
#undef jce__calloc_tracked
#define jce__calloc_tracked port_calloc
#define JCE_BUILD_JNI 1
#include JCE_TEST_JNI_HOST_OWNER
#undef jce__calloc_tracked
#undef jce__free_tracked

static jclass port_find_class(JNIEnv *env, const char *name)
{
    (void)env; (void)name; return (jclass)&engine_token;
}
static jmethodID port_method(JNIEnv *env, jclass type, const char *name,
                             const char *signature)
{
    (void)env; (void)type; (void)name; (void)signature;
    return (jmethodID)&engine_token;
}
static jstring port_string(JNIEnv *env, const char *value)
{
    (void)env; (void)value; return (jstring)&engine_token;
}
static jobject port_property(JNIEnv *env, jclass type, jmethodID method, ...)
{
    (void)env; (void)type; (void)method; ++port.properties;
    if (port.properties == port.fail_property) port.pending_exception = true;
    return port.property_values ? (jobject)&engine_token : NULL;
}
static void port_delete(JNIEnv *env, jobject value) { (void)env; (void)value; }
static const char *port_chars(JNIEnv *env, jstring value, void *copy)
{
    (void)env; (void)value; (void)copy; return "test-property";
}
static void port_release_chars(JNIEnv *env, jstring value, const char *chars)
{
    (void)env; (void)value; (void)chars; ++port.released_chars;
}
static jint port_throw(JNIEnv *env, jclass type, const char *message)
{
    (void)env; (void)type; ++port.exceptions;
    strncpy(port.exception_message, message, sizeof(port.exception_message) - 1);
    return 0;
}
static jboolean port_exception(JNIEnv *env)
{
    (void)env; return port.pending_exception ? 1 : 0;
}
static const struct JceTestJniTable jni_table = {
    port_find_class, port_method, port_string, port_property, port_delete,
    port_chars, port_release_chars, port_throw, port_exception
};
static JNIEnv jni = &jni_table;

void setUp(void)
{
    memset(&port, 0, sizeof port);
    port.create_owner = true;
    port.main_thread = true;
    terminal_token = "0123456789abcdef0123456789abcdef";
    heap_tracking = jce_alloc_track_snapshot(&heap_before);
}
void tearDown(void)
{
    /* Quarantine only the fixture's physical frees; production still calls its
     * actual release entry. Retained forced-quit memory is reclaimed here solely
     * to isolate CPU cases, never as a fabricated SDL continuation. */
    for (unsigned i = 0; i < port.allocation_count; ++i)
        JCE_FREE(port.allocations[i].memory);
    if (heap_tracking) {
        JceAllocTrack after;
        TEST_ASSERT_TRUE(jce_alloc_track_snapshot(&after));
        TEST_ASSERT_EQUAL_UINT64(heap_before.live_count, after.live_count);
        TEST_ASSERT_EQUAL_UINT64(heap_before.live_bytes, after.live_bytes);
    }
}

static void test_sdl_failed_boot_retains_until_spaced_recovery(void)
{
    void *context = NULL;
    SDL_Event event = {0};
    port.create_result = JCE_APP_FAILURE; port.refusals = 2;
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppInit(&context, 0, NULL));
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_EQUAL_PTR(port.sdl_memory, context);
    TEST_ASSERT_EQUAL_PTR(&engine_token, *port.owner_slot);
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppEvent(context, &event));
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppIterate(context));
    TEST_ASSERT_EQUAL_PTR(&engine_token, *port.owner_slot);
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppIterate(context));
    TEST_ASSERT_EQUAL_INT(SDL_APP_FAILURE, SDL_AppIterate(context));
    TEST_ASSERT_NULL(*port.owner_slot);
    TEST_ASSERT_EQUAL_UINT(3, port.destroys);
    TEST_ASSERT_EQUAL_UINT(3, port.delays);
    TEST_ASSERT_EQUAL_UINT(30, port.delayed);
    TEST_ASSERT_EQUAL_UINT(0, port.events + port.iterates);
    TEST_ASSERT_EQUAL_UINT(0, port.acknowledgements + port.frees);
    SDL_AppQuit(context, SDL_APP_FAILURE);
    TEST_ASSERT_EQUAL_UINT(1, port.releases);
    TEST_ASSERT_EQUAL_UINT(1, port.acknowledgements);
    TEST_ASSERT_EQUAL_UINT(1, port.frees);
    TEST_ASSERT_FALSE(port.ack_before_sdl_free);
    TEST_ASSERT_NOT_NULL(strstr(port.acknowledgement, "\"app_result\":2"));
}
static void test_sdl_terminal_event_does_not_bypass_drain(void)
{
    void *context = NULL;
    SDL_Event event = {0};
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppInit(&context, 0, NULL));
    TEST_ASSERT_NOT_NULL(context);
    port.event_result = JCE_APP_FAILURE; port.refusals = 1;
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppEvent(context, &event));
    port.event_result = JCE_APP_SUCCESS;
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppEvent(context, &event));
    TEST_ASSERT_EQUAL_UINT(1, port.events);
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppIterate(context));
    TEST_ASSERT_EQUAL_INT(SDL_APP_FAILURE, SDL_AppIterate(context));
    TEST_ASSERT_EQUAL_UINT(0, port.iterates);
    TEST_ASSERT_EQUAL_UINT(0, port.acknowledgements + port.frees);
    SDL_AppQuit(context, SDL_APP_FAILURE);
    TEST_ASSERT_EQUAL_UINT(1, port.acknowledgements);
    TEST_ASSERT_FALSE(port.ack_before_sdl_free);
}
static void test_sdl_success_ends_context_only_in_final_callback(void)
{
    void *context = NULL;
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppInit(&context, 0, NULL));
    TEST_ASSERT_NOT_NULL(context);
    port.iterate_result = JCE_APP_SUCCESS;
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppIterate(context));
    TEST_ASSERT_EQUAL_INT(SDL_APP_SUCCESS, SDL_AppIterate(context));
    TEST_ASSERT_EQUAL_UINT(1, port.iterates);
    TEST_ASSERT_EQUAL_UINT(0, port.acknowledgements + port.frees);
    SDL_AppQuit(context, SDL_APP_SUCCESS);
    TEST_ASSERT_EQUAL_UINT(1, port.releases);
    TEST_ASSERT_EQUAL_UINT(1, port.frees);
    TEST_ASSERT_EQUAL_UINT(1, port.acknowledgements);
    TEST_ASSERT_FALSE(port.ack_before_sdl_free);
    TEST_ASSERT_NOT_NULL(strstr(port.acknowledgement, "\"app_result\":1"));
}
static void test_sdl_forced_quit_refusal_is_unresolved_process_lifetime_retention(void)
{
    void *context = NULL;
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppInit(&context, 0, NULL));
    TEST_ASSERT_NOT_NULL(context);
    port.refusals = 1;
    SDL_AppQuit(context, SDL_APP_FAILURE);
    TEST_ASSERT_EQUAL_PTR(&engine_token, *port.owner_slot);
    TEST_ASSERT_EQUAL_UINT(1, port.warnings);
    TEST_ASSERT_EQUAL_UINT(1, port.destroys);
    TEST_ASSERT_EQUAL_UINT(0, port.frees + port.releases + port.acknowledgements);
    /* Quit is SDL's final callback: there is no fabricated post-Quit retry.
     * The fixture alone reclaims quarantined memory during case isolation. */
}
static void test_sdl_no_owner_failure_and_duplicate_init(void)
{
    void *context = NULL;
    port.create_result = JCE_APP_FAILURE; port.create_owner = false;
    TEST_ASSERT_EQUAL_INT(SDL_APP_FAILURE, SDL_AppInit(&context, 0, NULL));
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_NULL(*port.owner_slot);
    SDL_AppQuit(context, SDL_APP_FAILURE);
    context = NULL; /* A separate simulated SDL application lifetime. */
    port.create_result = JCE_APP_CONTINUE; port.create_owner = true;
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppInit(&context, 0, NULL));
    TEST_ASSERT_NOT_NULL(context);
    void *original = context;
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppInit(&context, 0, NULL));
    TEST_ASSERT_EQUAL_PTR(original, context);
    TEST_ASSERT_EQUAL_UINT(2, port.creates);
    TEST_ASSERT_EQUAL_UINT(2, port.allocation_count);
    TEST_ASSERT_EQUAL_INT(SDL_APP_FAILURE, SDL_AppIterate(context));
    SDL_AppQuit(context, SDL_APP_FAILURE);
    TEST_ASSERT_EQUAL_UINT(2, port.frees);
    TEST_ASSERT_EQUAL_UINT(1, port.acknowledgements);
}
static void test_sdl_context_allocation_failure_and_null_callbacks_create_no_owner(void)
{
    void *context = NULL;
    port.fail_bridge_alloc = true;
    TEST_ASSERT_EQUAL_INT(SDL_APP_FAILURE, SDL_AppInit(&context, 0, NULL));
    TEST_ASSERT_NULL(context);
    TEST_ASSERT_EQUAL_INT(SDL_APP_FAILURE, SDL_AppInit(NULL, 0, NULL));
    TEST_ASSERT_EQUAL_UINT(0, port.creates + port.allocation_count);
    TEST_ASSERT_EQUAL_INT(SDL_APP_FAILURE, SDL_AppIterate(NULL));
    SDL_Event event = {0};
    TEST_ASSERT_EQUAL_INT(SDL_APP_FAILURE, SDL_AppEvent(NULL, &event));
    SDL_AppQuit(NULL, SDL_APP_FAILURE);
    TEST_ASSERT_EQUAL_UINT(0, port.acknowledgements + port.frees + port.destroys);
}
static void test_sdl_foreign_callbacks_never_borrow_or_mutate_context(void)
{
    void *context = NULL;
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppInit(&context, 0, NULL));
    TEST_ASSERT_NOT_NULL(context);
    port.main_thread = false;
    SDL_Event event = {0};
    TEST_ASSERT_EQUAL_INT(SDL_APP_FAILURE, SDL_AppEvent(context, &event));
    TEST_ASSERT_EQUAL_INT(SDL_APP_FAILURE, SDL_AppIterate(context));
    SDL_AppQuit(context, SDL_APP_FAILURE);
    TEST_ASSERT_EQUAL_UINT(0, port.events + port.iterates + port.destroys + port.frees);
    port.main_thread = true;
    SDL_AppQuit(context, SDL_APP_SUCCESS);
    TEST_ASSERT_EQUAL_UINT(1, port.acknowledgements);
    TEST_ASSERT_NOT_NULL(strstr(port.acknowledgement, "\"app_result\":1"));
}
static void test_sdl_reentrant_final_callback_latches_failure_without_double_release(void)
{
    void *context = NULL;
    TEST_ASSERT_EQUAL_INT(SDL_APP_CONTINUE, SDL_AppInit(&context, 0, NULL));
    TEST_ASSERT_NOT_NULL(context);
    port.reenter_quit = true;
    SDL_AppQuit(context, SDL_APP_SUCCESS);
    TEST_ASSERT_TRUE(port.reentered_quit);
    TEST_ASSERT_EQUAL_UINT(1, port.destroys);
    TEST_ASSERT_EQUAL_UINT(1, port.releases);
    TEST_ASSERT_EQUAL_UINT(1, port.frees);
    TEST_ASSERT_EQUAL_UINT(1, port.acknowledgements);
    TEST_ASSERT_FALSE(port.ack_before_sdl_free);
    TEST_ASSERT_NOT_NULL(strstr(port.acknowledgement, "\"app_result\":2"));
}
static void test_core_heap_release_after_logger_and_sdl_shutdown_stays_balanced(void)
{
    JceAllocTrack before, after;
    const bool tracking = jce_alloc_track_snapshot(&before);
    void *context = JCE_CALLOC(1, 64);
    TEST_ASSERT_NOT_NULL(context);
    jce_log_init();
    jce_trace_set_enabled(false);
    jce_log_shutdown();
    SDL_Quit();
    JCE_FREE(context);
    if (tracking) {
        TEST_ASSERT_TRUE(jce_alloc_track_snapshot(&after));
        TEST_ASSERT_EQUAL_UINT64(before.live_count, after.live_count);
        TEST_ASSERT_EQUAL_UINT64(before.live_bytes, after.live_bytes);
    }
}
static void test_jni_failed_boot_handle_survives_repeated_refusal(void)
{
    port.create_result = JCE_APP_FAILURE; port.refusals = 2;
    jlong handle = Java_com_jce_JceRuntime_nativeCreate(&jni, NULL);
    TEST_ASSERT_NOT_EQUAL(0, handle);
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE,
        Java_com_jce_JceRuntime_nativeLastResult(&jni, NULL, handle));
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE,
        Java_com_jce_JceRuntime_nativeIterate(&jni, NULL, handle));
    for (unsigned i = 1; i <= 2; ++i) {
        Java_com_jce_JceRuntime_nativeDestroy(&jni, NULL, handle);
        TEST_ASSERT_EQUAL_UINT(i, port.exceptions);
        TEST_ASSERT_EQUAL_UINT(0, port.frees);
        TEST_ASSERT_EQUAL_PTR(&engine_token, jce_bridge_from_handle(handle)->engine);
        TEST_ASSERT_NOT_NULL(strstr(port.exception_message, "retry close()"));
    }
    Java_com_jce_JceRuntime_nativeDestroy(&jni, NULL, handle);
    TEST_ASSERT_EQUAL_UINT(1, port.frees);
    TEST_ASSERT_EQUAL_UINT(1, port.releases);
    TEST_ASSERT_EQUAL_UINT(0, port.polls + port.events + port.iterates);
    TEST_ASSERT_EQUAL_UINT(1, port.acknowledgements);
    TEST_ASSERT_FALSE(port.ack_before_bridge_free);
}
static void test_jni_manual_close_blocks_subsequent_gameplay(void)
{
    jlong handle = Java_com_jce_JceRuntime_nativeCreate(&jni, NULL);
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE,
        Java_com_jce_JceRuntime_nativeIterate(&jni, NULL, handle));
    port.refusals = 1;
    Java_com_jce_JceRuntime_nativeDestroy(&jni, NULL, handle);
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS,
        Java_com_jce_JceRuntime_nativeIterate(&jni, NULL, handle));
    TEST_ASSERT_EQUAL_UINT(1, port.polls);
    TEST_ASSERT_EQUAL_UINT(1, port.iterates);
    Java_com_jce_JceRuntime_nativeDestroy(&jni, NULL, handle);
    TEST_ASSERT_EQUAL_UINT(1, port.frees);
    TEST_ASSERT_EQUAL_UINT(1, port.acknowledgements);
}
static void test_jni_no_owner_and_bridge_oom_do_not_leak(void)
{
    port.create_result = JCE_APP_FAILURE; port.create_owner = false;
    TEST_ASSERT_EQUAL_INT64(0, Java_com_jce_JceRuntime_nativeCreate(&jni, NULL));
    TEST_ASSERT_EQUAL_UINT(1, port.frees);
    port.fail_bridge_alloc = true;
    TEST_ASSERT_EQUAL_INT64(0, Java_com_jce_JceRuntime_nativeCreate(&jni, NULL));
    TEST_ASSERT_EQUAL_UINT(1, port.creates);
    Java_com_jce_JceRuntime_nativeDestroy(&jni, NULL, 0);
    TEST_ASSERT_EQUAL_UINT(1, port.frees);
    TEST_ASSERT_EQUAL_UINT(0, port.acknowledgements);
}
static void test_jni_property_exception_precedes_owner_creation(void)
{
    port.fail_property = 1; port.property_values = true;
    TEST_ASSERT_EQUAL_INT64(0, Java_com_jce_JceRuntime_nativeCreate(&jni, NULL));
    TEST_ASSERT_EQUAL_UINT(0, port.creates);
    TEST_ASSERT_NULL(port.bridge_memory);
    port.properties = 0; port.fail_property = 2; port.pending_exception = false;
    TEST_ASSERT_EQUAL_INT64(0, Java_com_jce_JceRuntime_nativeCreate(&jni, NULL));
    TEST_ASSERT_EQUAL_UINT(0, port.creates);
    TEST_ASSERT_EQUAL_UINT(1, port.released_chars);
    TEST_ASSERT_NULL(port.bridge_memory);
}
static void test_immediate_failed_boot_acknowledges_only_an_admitted_session(void)
{
    void *context = NULL;
    port.create_owner = false; port.create_result = JCE_APP_FAILURE;
    TEST_ASSERT_EQUAL_INT(SDL_APP_FAILURE, SDL_AppInit(&context, 0, NULL));
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_EQUAL_UINT(0, port.acknowledgements);
    SDL_AppQuit(context, SDL_APP_FAILURE);
    TEST_ASSERT_EQUAL_UINT(0, port.acknowledgements);
    context = NULL;
    port.admitted_without_owner = true;
    TEST_ASSERT_EQUAL_INT(SDL_APP_FAILURE, SDL_AppInit(&context, 0, NULL));
    TEST_ASSERT_NOT_NULL(context);
    TEST_ASSERT_EQUAL_UINT(0, port.acknowledgements);
    SDL_AppQuit(context, SDL_APP_FAILURE);
    TEST_ASSERT_EQUAL_UINT(1, port.acknowledgements);
    TEST_ASSERT_FALSE(port.ack_before_sdl_free);
    TEST_ASSERT_EQUAL_INT64(0, Java_com_jce_JceRuntime_nativeCreate(&jni, NULL));
    TEST_ASSERT_EQUAL_UINT(2, port.acknowledgements);
    TEST_ASSERT_FALSE(port.ack_before_bridge_free);
}
static void test_optional_terminal_ack_rejects_early_duplicate_and_stale_tickets(void)
{
    terminal_token = NULL;
    uint64_t disabled_before = jce_engine_terminal_cursor_internal();
    jce_engine_terminal_begin_internal();
    jce_engine_terminal_finish_internal();
    TEST_ASSERT_EQUAL_UINT64(0, jce_engine_terminal_after_create_internal(disabled_before));
    TEST_ASSERT_EQUAL_UINT(0, port.acknowledgements);
    terminal_token = "INVALID";
    jce_engine_terminal_begin_internal();
    jce_engine_terminal_finish_internal();
    TEST_ASSERT_FALSE(jce_engine_terminal_emit_internal(0, "sdl", 1, true));
    terminal_token = "0123456789abcdef0123456789abcdef";
    uint64_t before = jce_engine_terminal_cursor_internal();
    jce_engine_terminal_begin_internal();
    uint64_t session = jce_engine_terminal_after_create_internal(before);
    TEST_ASSERT_TRUE(session > before);
    TEST_ASSERT_FALSE(jce_engine_terminal_emit_internal(session, "sdl", 1, true));
    jce_engine_terminal_log_internal(JCE_LOG_FINAL_CLOSE_API_FAILED);
    jce_engine_terminal_finish_internal();
    TEST_ASSERT_FALSE(jce_engine_terminal_emit_internal(session, "sdl", 1, false));
    TEST_ASSERT_TRUE(jce_engine_terminal_emit_internal(session, "sdl", 2, true));
    TEST_ASSERT_FALSE(jce_engine_terminal_emit_internal(session, "sdl", 2, true));
    TEST_ASSERT_NOT_NULL(strstr(port.acknowledgement, "\"log_file_close\":\"api_failed\""));
    jce_engine_terminal_begin_internal();
    jce_engine_terminal_finish_internal();
    TEST_ASSERT_FALSE(jce_engine_terminal_emit_internal(session, "sdl", 1, true));
    TEST_ASSERT_EQUAL_UINT(1, port.acknowledgements);
}
static void test_partial_terminal_output_is_not_replayed(void)
{
    uint64_t before = jce_engine_terminal_cursor_internal();
    jce_engine_terminal_begin_internal();
    uint64_t session = jce_engine_terminal_after_create_internal(before);
    jce_engine_terminal_log_internal(JCE_LOG_FINAL_CLOSE_NO_FILE);
    jce_engine_terminal_finish_internal();
    port.fail_ack_write = true;
    TEST_ASSERT_FALSE(jce_engine_terminal_emit_internal(session, "sdl", 1, true));
    TEST_ASSERT_EQUAL_STRING("JCE_TERMINAL_ACK ", port.acknowledgement);
    port.fail_ack_write = false;
    TEST_ASSERT_FALSE(jce_engine_terminal_emit_internal(session, "sdl", 1, true));
    TEST_ASSERT_EQUAL_UINT(1, port.acknowledgements);
    TEST_ASSERT_EQUAL_STRING("JCE_TERMINAL_ACK ", port.acknowledgement);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sdl_failed_boot_retains_until_spaced_recovery);
    RUN_TEST(test_sdl_terminal_event_does_not_bypass_drain);
    RUN_TEST(test_sdl_success_ends_context_only_in_final_callback);
    RUN_TEST(test_sdl_forced_quit_refusal_is_unresolved_process_lifetime_retention);
    RUN_TEST(test_sdl_no_owner_failure_and_duplicate_init);
    RUN_TEST(test_sdl_context_allocation_failure_and_null_callbacks_create_no_owner);
    RUN_TEST(test_sdl_foreign_callbacks_never_borrow_or_mutate_context);
    RUN_TEST(test_sdl_reentrant_final_callback_latches_failure_without_double_release);
    RUN_TEST(test_core_heap_release_after_logger_and_sdl_shutdown_stays_balanced);
    RUN_TEST(test_jni_failed_boot_handle_survives_repeated_refusal);
    RUN_TEST(test_jni_manual_close_blocks_subsequent_gameplay);
    RUN_TEST(test_jni_no_owner_and_bridge_oom_do_not_leak);
    RUN_TEST(test_jni_property_exception_precedes_owner_creation);
    RUN_TEST(test_immediate_failed_boot_acknowledges_only_an_admitted_session);
    RUN_TEST(test_optional_terminal_ack_rejects_early_duplicate_and_stale_tickets);
    RUN_TEST(test_partial_terminal_output_is_not_replayed);
    return UNITY_END();
}
